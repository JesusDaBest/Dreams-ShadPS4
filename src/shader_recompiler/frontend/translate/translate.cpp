// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "common/io_file.h"
#include "common/path_util.h"
#include "core/emulator_settings.h"
#include "core/libraries/kernel/process.h"
#include "shader_recompiler/dreams_compat.h"
#include "shader_recompiler/frontend/decode.h"
#include "shader_recompiler/frontend/fetch_shader.h"
#include "shader_recompiler/frontend/translate/translate.h"
#include "shader_recompiler/info.h"
#include "shader_recompiler/ir/attribute.h"
#include "shader_recompiler/ir/reg.h"
#include "shader_recompiler/ir/reinterpret.h"
#include "shader_recompiler/profile.h"
#include "shader_recompiler/runtime_info.h"
#include "video_core/amdgpu/resource.h"

#include <array>
#include <cstdlib>
#include <numbers>
#include <string_view>
#include <magic_enum/magic_enum.hpp>

namespace Shader::Gcn {

static std::pair<IR::U32, IR::U32> ThreadBitMaskDwords(IR::IREmitter& ir, const IR::U1& value) {
    const IR::Value ballot = ir.Ballot(value);
    const IR::U32 lo{ir.CompositeExtract(ballot, 0)};
    const IR::U32 hi{ir.CompositeExtract(ballot, 1)};
    return {lo, hi};
}

static IR::U64 PackThreadBitMask(IR::IREmitter& ir, const IR::U1& value) {
    const auto [lo, hi] = ThreadBitMaskDwords(ir, value);
    return ir.PackUint2x32(ir.CompositeConstruct(lo, hi));
}

static IR::U1 ThreadBitFromDwordMask(IR::IREmitter& ir, u32 mask) {
    if (mask == 0) {
        return ir.Imm1(false);
    }
    if (mask == std::numeric_limits<u32>::max()) {
        return ir.Imm1(true);
    }

    const IR::U32 lane = ir.BitwiseAnd(ir.LaneId(), ir.Imm32(0x3f));
    const IR::U1 is_low_lane = ir.ILessThan(lane, ir.Imm32(32), false);
    const IR::U32 shift = ir.BitwiseAnd(lane, ir.Imm32(0x1f));
    const IR::U32 bit = ir.BitwiseAnd(ir.ShiftRightLogical(ir.Imm32(mask), shift), ir.Imm32(1));
    return ir.LogicalAnd(is_low_lane, ir.INotEqual(bit, ir.Imm32(0)));
}

static IR::VectorReg IterateBarycentrics(const RuntimeInfo& runtime_info, auto&& set_attribute) {
    if (runtime_info.stage != Stage::Fragment) {
        return IR::VectorReg::V0;
    }
    u32 dst_vreg{};
    if (runtime_info.fs_info.addr_flags.persp_sample_ena) {
        set_attribute(dst_vreg++, IR::Attribute::BaryCoordSmoothSample, 0); // I
        set_attribute(dst_vreg++, IR::Attribute::BaryCoordSmoothSample, 1); // J
    }
    if (runtime_info.fs_info.addr_flags.persp_center_ena) {
        set_attribute(dst_vreg++, IR::Attribute::BaryCoordSmooth, 0); // I
        set_attribute(dst_vreg++, IR::Attribute::BaryCoordSmooth, 1); // J
    }
    if (runtime_info.fs_info.addr_flags.persp_centroid_ena) {
        set_attribute(dst_vreg++, IR::Attribute::BaryCoordSmoothCentroid, 0); // I
        set_attribute(dst_vreg++, IR::Attribute::BaryCoordSmoothCentroid, 1); // J
    }
    if (runtime_info.fs_info.addr_flags.persp_pull_model_ena) {
        set_attribute(dst_vreg++, IR::Attribute::BaryCoordPullModel, 0); // I/W
        set_attribute(dst_vreg++, IR::Attribute::BaryCoordPullModel, 1); // J/W
        set_attribute(dst_vreg++, IR::Attribute::BaryCoordPullModel, 2); // 1/W
    }
    if (runtime_info.fs_info.addr_flags.linear_sample_ena) {
        set_attribute(dst_vreg++, IR::Attribute::BaryCoordNoPerspSample, 0); // I
        set_attribute(dst_vreg++, IR::Attribute::BaryCoordNoPerspSample, 1); // J
    }
    if (runtime_info.fs_info.addr_flags.linear_center_ena) {
        set_attribute(dst_vreg++, IR::Attribute::BaryCoordNoPersp, 0); // I
        set_attribute(dst_vreg++, IR::Attribute::BaryCoordNoPersp, 1); // J
    }
    if (runtime_info.fs_info.addr_flags.linear_centroid_ena) {
        set_attribute(dst_vreg++, IR::Attribute::BaryCoordNoPerspCentroid, 0); // I
        set_attribute(dst_vreg++, IR::Attribute::BaryCoordNoPerspCentroid, 1); // J
    }
    if (runtime_info.fs_info.addr_flags.line_stipple_tex_ena) {
        ++dst_vreg;
    }
    return IR::VectorReg(dst_vreg);
}

Translator::Translator(Info& info_, const RuntimeInfo& runtime_info_, const Profile& profile_)
    : info{info_}, runtime_info{runtime_info_}, profile{profile_},
      next_vgpr_num{runtime_info.num_allocated_vgprs} {
    IterateBarycentrics(runtime_info, [this](u32 vreg, IR::Attribute attrib, u32) {
        vgpr_to_interp[vreg] = attrib;
    });
}

void Translator::EmitPrologue(IR::Block* first_block) {
    ir = IR::IREmitter(*first_block, first_block->begin());

    ir.Prologue();
    ir.SetExec(ir.Imm1(true));

    // Initialize user data.
    IR::ScalarReg dst_sreg = IR::ScalarReg::S0;
    for (u32 i = 0; i < runtime_info.num_user_data; i++) {
        ir.SetScalarReg(dst_sreg, ir.GetUserData(dst_sreg));
        ++dst_sreg;
    }

    IR::VectorReg dst_vreg = IR::VectorReg::V0;
    switch (info.l_stage) {
    case LogicalStage::Vertex:
        // v0: vertex ID, always present
        ir.SetVectorReg(dst_vreg++, ir.GetAttributeU32(IR::Attribute::VertexId));
        if (info.stage == Stage::Local) {
            // v1: rel patch ID
            if (runtime_info.num_input_vgprs > 0) {
                ir.SetVectorReg(dst_vreg++, ir.Imm32(0));
            }
            // v2: unknown
            if (runtime_info.num_input_vgprs > 1) {
                ++dst_vreg;
            }
            // v3: instance ID, plain
            if (runtime_info.num_input_vgprs > 2) {
                ir.SetVectorReg(dst_vreg++, ir.GetAttributeU32(IR::Attribute::InstanceId));
            }
        } else {
            // v1: instance ID, step rate 0
            if (runtime_info.num_input_vgprs > 0) {
                if (runtime_info.vs_info.step_rate_0 != 0) {
                    ir.SetVectorReg(dst_vreg++,
                                    ir.IDiv(ir.GetAttributeU32(IR::Attribute::InstanceId),
                                            ir.Imm32(runtime_info.vs_info.step_rate_0)));
                } else {
                    ir.SetVectorReg(dst_vreg++, ir.Imm32(0));
                }
            }
            // v2: instance ID, step rate 1
            if (runtime_info.num_input_vgprs > 1) {
                if (runtime_info.vs_info.step_rate_1 != 0) {
                    ir.SetVectorReg(dst_vreg++,
                                    ir.IDiv(ir.GetAttributeU32(IR::Attribute::InstanceId),
                                            ir.Imm32(runtime_info.vs_info.step_rate_1)));
                } else {
                    ir.SetVectorReg(dst_vreg++, ir.Imm32(0));
                }
            }
            // v3: instance ID, plain
            if (runtime_info.num_input_vgprs > 2) {
                ir.SetVectorReg(dst_vreg++, ir.GetAttributeU32(IR::Attribute::InstanceId));
            }
        }
        break;
    case LogicalStage::Fragment:
        dst_vreg =
            IterateBarycentrics(runtime_info, [this](u32 vreg, IR::Attribute attrib, u32 comp) {
                if (profile.supports_amd_shader_explicit_vertex_parameter ||
                    profile.supports_fragment_shader_barycentric) {
                    ir.SetVectorReg(IR::VectorReg(vreg), ir.GetAttribute(attrib, comp));
                }
            });
        if (runtime_info.fs_info.addr_flags.pos_x_float_ena) {
            if (runtime_info.fs_info.en_flags.pos_x_float_ena) {
                ir.SetVectorReg(dst_vreg++, ir.GetAttribute(IR::Attribute::FragCoord, 0));
            } else {
                ir.SetVectorReg(dst_vreg++, ir.Imm32(0.0f));
            }
        }
        if (runtime_info.fs_info.addr_flags.pos_y_float_ena) {
            if (runtime_info.fs_info.en_flags.pos_y_float_ena) {
                ir.SetVectorReg(dst_vreg++, ir.GetAttribute(IR::Attribute::FragCoord, 1));
            } else {
                ir.SetVectorReg(dst_vreg++, ir.Imm32(0.0f));
            }
        }
        if (runtime_info.fs_info.addr_flags.pos_z_float_ena) {
            if (runtime_info.fs_info.en_flags.pos_z_float_ena) {
                ir.SetVectorReg(dst_vreg++, ir.GetAttribute(IR::Attribute::FragCoord, 2));
            } else {
                ir.SetVectorReg(dst_vreg++, ir.Imm32(0.0f));
            }
        }
        if (runtime_info.fs_info.addr_flags.pos_w_float_ena) {
            if (runtime_info.fs_info.en_flags.pos_w_float_ena) {
                ir.SetVectorReg(dst_vreg++,
                                ir.FPRecip(ir.GetAttribute(IR::Attribute::FragCoord, 3)));
            } else {
                ir.SetVectorReg(dst_vreg++, ir.Imm32(0.0f));
            }
        }
        if (runtime_info.fs_info.addr_flags.front_face_ena) {
            if (runtime_info.fs_info.en_flags.front_face_ena) {
                ir.SetVectorReg(dst_vreg++, ir.GetAttributeU32(IR::Attribute::IsFrontFace));
            } else {
                ir.SetVectorReg(dst_vreg++, ir.Imm32(0));
            }
        }
        if (runtime_info.fs_info.addr_flags.ancillary_ena) {
            if (runtime_info.fs_info.en_flags.ancillary_ena) {
                ir.SetVectorReg(dst_vreg++, ir.GetAttributeU32(IR::Attribute::PackedAncillary));
            } else {
                ir.SetVectorReg(dst_vreg++, ir.Imm32(0));
            }
        }
        // SAMPLE_COVERAGE precedes POS_FIXED_PT in the hardware VGPR layout. Its value is not
        // currently exposed by the backend, but the slot still has to be reserved so later inputs
        // retain their hardware register numbers.
        if (runtime_info.fs_info.addr_flags.sample_coverage_ena) {
            if (!runtime_info.fs_info.en_flags.sample_coverage_ena) {
                ir.SetVectorReg(dst_vreg, ir.Imm32(0));
            }
            ++dst_vreg;
        }
        if (runtime_info.fs_info.addr_flags.pos_fixed_pt_ena) {
            if (runtime_info.fs_info.en_flags.pos_fixed_pt_ena) {
                // POS_FIXED_PT packs the integer pixel coordinate as X in bits 0..15 and Y in
                // bits 16..31. Vulkan FragCoord uses half-integer pixel centers, so unsigned
                // conversion yields the same upper-left-origin integer coordinates.
                const IR::U32 pixel_x{
                    ir.ConvertFToU(32, ir.GetAttribute(IR::Attribute::FragCoord, 0))};
                const IR::U32 pixel_y{
                    ir.ConvertFToU(32, ir.GetAttribute(IR::Attribute::FragCoord, 1))};
                const IR::U32 packed_x = ir.BitFieldInsert(
                    ir.Imm32(0), pixel_x, ir.Imm32(0), ir.Imm32(16));
                const IR::U32 packed_xy = ir.BitFieldInsert(
                    packed_x, pixel_y, ir.Imm32(16), ir.Imm32(16));
                ir.SetVectorReg(dst_vreg++, packed_xy);
            } else {
                ir.SetVectorReg(dst_vreg++, ir.Imm32(0));
            }
        }
        break;
    case LogicalStage::TessellationControl: {
        ir.SetVectorReg(IR::VectorReg::V0, ir.GetAttributeU32(IR::Attribute::PrimitiveId));
        // Should be laid out like:
        // [0:8]: patch id within VGT
        // [8:12]: output control point id
        ir.SetVectorReg(IR::VectorReg::V1,
                        ir.GetAttributeU32(IR::Attribute::PackedHullInvocationInfo));

        if (runtime_info.hs_info.offchip_lds_enable) {
            // No off-chip tessellation has been observed yet. If this survives dead code elim,
            // revisit
            ir.SetScalarReg(dst_sreg++, ir.GetAttributeU32(IR::Attribute::OffChipLdsBase));
        }
        ir.SetScalarReg(dst_sreg++, ir.GetAttributeU32(IR::Attribute::TessFactorsBufferBase));

        break;
    }
    case LogicalStage::TessellationEval:
        ir.SetVectorReg(IR::VectorReg::V0,
                        ir.GetAttribute(IR::Attribute::TessellationEvaluationPointU));
        ir.SetVectorReg(IR::VectorReg::V1,
                        ir.GetAttribute(IR::Attribute::TessellationEvaluationPointV));
        // V2 is similar to PrimitiveID but not the same. It seems to only be used in
        // compiler-generated address calculations. Its probably the patch id within the
        // patches running locally on a given VGT (or CU, whichever is the granularity of LDS
        // memory)
        // Set to 0. See explanation in comment describing hull/domain passes
        ir.SetVectorReg(IR::VectorReg::V2, ir.Imm32(0u));
        // V3 is the actual PrimitiveID as intended by the shader author.
        ir.SetVectorReg(IR::VectorReg::V3, ir.GetAttributeU32(IR::Attribute::PrimitiveId));
        break;
    case LogicalStage::Compute:
        ir.SetVectorReg(dst_vreg++, ir.GetAttributeU32(IR::Attribute::LocalInvocationId, 0));
        ir.SetVectorReg(dst_vreg++, ir.GetAttributeU32(IR::Attribute::LocalInvocationId, 1));
        ir.SetVectorReg(dst_vreg++, ir.GetAttributeU32(IR::Attribute::LocalInvocationId, 2));

        if (runtime_info.cs_info.tgid_enable[0]) {
            ir.SetScalarReg(dst_sreg++, ir.GetAttributeU32(IR::Attribute::WorkgroupId, 0));
        }
        if (runtime_info.cs_info.tgid_enable[1]) {
            ir.SetScalarReg(dst_sreg++, ir.GetAttributeU32(IR::Attribute::WorkgroupId, 1));
        }
        if (runtime_info.cs_info.tgid_enable[2]) {
            ir.SetScalarReg(dst_sreg++, ir.GetAttributeU32(IR::Attribute::WorkgroupId, 2));
        }
        if (runtime_info.cs_info.tg_size_enable ||
            runtime_info.cs_info.ordered_append_enable) {
            const auto& size = runtime_info.cs_info.workgroup_size;
            const u32 threads = size[0] * size[1] * size[2];
            const u32 waves = (threads + 63) / 64;
            IR::U32 workgroup_info = ir.Imm32(waves & 0x3f);
            if (runtime_info.cs_info.ordered_append_enable &&
                info.pgm_hash == DreamsCompat::TraversalShader && waves == 1) {
                // Liverpool packs the ordered-append term in bits 6..16 and marks the first wave
                // in bit 31. This shader has one wave per x-only workgroup, and dispatchBase keeps
                // WorkgroupId.x equal to the original guest ID while serializing it.
                const IR::U32 workgroup_x =
                    ir.GetAttributeU32(IR::Attribute::WorkgroupId, 0);
                const IR::U32 ordered_term = ir.ShiftLeftLogical(
                    ir.BitwiseAnd(workgroup_x, ir.Imm32(0x7ff)), ir.Imm32(6));
                workgroup_info =
                    ir.BitwiseOr(workgroup_info,
                                 ir.BitwiseOr(ordered_term, ir.Imm32(0x80000000U)));
            }
            ir.SetScalarReg(dst_sreg++, workgroup_info);
        }
        break;
    case LogicalStage::Geometry:
        // The GS wave receives one ES vertex offset per input primitive vertex in V0-V6, with
        // the primitive id in V2. The offset count is a property of the input primitive type;
        // adjacency primitives carry up to 6 vertices.
        switch (runtime_info.gs_info.in_primitive) {
        case AmdGpu::PrimitiveType::AdjTriangleList:
        case AmdGpu::PrimitiveType::AdjTriangleStrip:
            ir.SetVectorReg(IR::VectorReg::V6, ir.Imm32(5u)); // vertex 5
            ir.SetVectorReg(IR::VectorReg::V5, ir.Imm32(4u)); // vertex 4
            [[fallthrough]];
        case AmdGpu::PrimitiveType::AdjLineList:
        case AmdGpu::PrimitiveType::AdjLineStrip:
            ir.SetVectorReg(IR::VectorReg::V4, ir.Imm32(3u)); // vertex 3
            [[fallthrough]];
        case AmdGpu::PrimitiveType::TriangleList:
        case AmdGpu::PrimitiveType::TriangleStrip:
        case AmdGpu::PrimitiveType::RectList:
            ir.SetVectorReg(IR::VectorReg::V3, ir.Imm32(2u)); // vertex 2
            [[fallthrough]];
        case AmdGpu::PrimitiveType::LineList:
        case AmdGpu::PrimitiveType::LineStrip:
            ir.SetVectorReg(IR::VectorReg::V1, ir.Imm32(1u)); // vertex 1
            [[fallthrough]];
        default:
            ir.SetVectorReg(IR::VectorReg::V0, ir.Imm32(0u)); // vertex 0
            break;
        }
        ir.SetVectorReg(IR::VectorReg::V2, ir.GetAttributeU32(IR::Attribute::PrimitiveId));
        break;
    default:
        UNREACHABLE_MSG("Unknown shader stage");
    }

    // Clear any scratch vgpr mappings for next shader.
    vgpr_map.clear();
}

IR::VectorReg Translator::GetScratchVgpr(u32 offset) {
    const auto [it, is_new] = vgpr_map.try_emplace(offset);
    if (is_new) {
        ASSERT_MSG(next_vgpr_num < 256, "Out of VGPRs");
        const auto new_vgpr = static_cast<IR::VectorReg>(next_vgpr_num++);
        it->second = new_vgpr;
    }
    return it->second;
};

IR::U1 Translator::GetThreadBitScalarReg(IR::ScalarReg reg) {
    const IR::U32 lane{ir.BitwiseAnd(ir.LaneId(), ir.Imm32(0x3f))};
    const IR::U1 high_lane{ir.IGreaterThanEqual(lane, ir.Imm32(32), false)};
    const IR::U32 word{ir.Select(high_lane, ir.GetScalarReg(reg + 1), ir.GetScalarReg(reg))};
    const IR::U32 shift{ir.BitwiseAnd(lane, ir.Imm32(0x1f))};
    const IR::U32 bit{ir.BitwiseAnd(ir.ShiftRightLogical(word, shift), ir.Imm32(1))};
    return ir.INotEqual(bit, ir.Imm32(0));
}

IR::U1 Translator::GetSrc1(const InstOperand& operand) {
    switch (operand.field) {
    case OperandField::VccLo:
        return ir.GetVcc();
    case OperandField::ExecLo:
        return ir.GetExec();
    case OperandField::ScalarGPR:
        return GetThreadBitScalarReg(IR::ScalarReg(operand.code));
    case OperandField::ConstZero:
        return ir.Imm1(false);
    case OperandField::SignedConstIntNeg:
        ASSERT_MSG(-s32(operand.code) + SignedConstIntNegMin - 1 == -1,
                   "SignedConstIntNeg must be -1");
        return ir.Imm1(true);
    case OperandField::LiteralConst:
        return ThreadBitFromDwordMask(ir, operand.code);
    default:
        UNREACHABLE_MSG("Unknown field {}", u32(operand.field));
    }
}

template <typename T>
T Translator::GetSrc(const InstOperand& operand) {
    constexpr bool is_float = std::is_same_v<T, IR::F32>;

    const auto get_imm = [&](auto value) -> T {
        if constexpr (is_float) {
            return ir.Imm32(std::bit_cast<float>(value));
        } else {
            return ir.Imm32(std::bit_cast<u32>(value));
        }
    };

    T value{};
    switch (operand.field) {
    case OperandField::ScalarGPR:
        value = ir.GetScalarReg<T>(IR::ScalarReg(operand.code));
        break;
    case OperandField::VectorGPR:
        value = ir.GetVectorReg<T>(IR::VectorReg(operand.code));
        break;
    case OperandField::ConstZero:
        value = get_imm(0U);
        break;
    case OperandField::SignedConstIntPos:
        value = get_imm(operand.code - SignedConstIntPosMin + 1);
        break;
    case OperandField::SignedConstIntNeg:
        value = get_imm(-s32(operand.code) + SignedConstIntNegMin - 1);
        break;
    case OperandField::LiteralConst:
        value = get_imm(operand.code);
        break;
    case OperandField::ConstFloatPos_1_0:
        value = get_imm(1.f);
        break;
    case OperandField::ConstFloatPos_0_5:
        value = get_imm(0.5f);
        break;
    case OperandField::ConstFloatPos_2_0:
        value = get_imm(2.0f);
        break;
    case OperandField::ConstFloatPos_4_0:
        value = get_imm(4.0f);
        break;
    case OperandField::ConstFloatNeg_0_5:
        value = get_imm(-0.5f);
        break;
    case OperandField::ConstFloatNeg_1_0:
        value = get_imm(-1.0f);
        break;
    case OperandField::ConstFloatNeg_2_0:
        value = get_imm(-2.0f);
        break;
    case OperandField::ConstFloatNeg_4_0:
        value = get_imm(-4.0f);
        break;
    case OperandField::Inv2Pi:
        value = get_imm(static_cast<float>(1.0f / (2.0f * std::numbers::pi)));
        break;
    case OperandField::Sdwa:
        UNREACHABLE_MSG("unhandled SDWA");
    case OperandField::Dpp:
        UNREACHABLE_MSG("unhandled DPP");
    case OperandField::VccLo:
        if constexpr (is_float) {
            value = ir.BitCast<IR::F32>(ir.GetVccLo());
        } else {
            value = ir.GetVccLo();
        }
        break;
    case OperandField::VccHi:
        if constexpr (is_float) {
            value = ir.BitCast<IR::F32>(ir.GetVccHi());
        } else {
            value = ir.GetVccHi();
        }
        break;
    case OperandField::M0:
        if constexpr (is_float) {
            value = ir.BitCast<IR::F32>(ir.GetM0());
        } else {
            value = ir.GetM0();
        }
        break;
    case OperandField::Scc:
        if constexpr (is_float) {
            UNREACHABLE();
        } else {
            value = ir.BitCast<IR::U32>(ir.GetScc());
        }
        break;
    default:
        UNREACHABLE_MSG("unexpected operand: {}", std::to_underlying(operand.field));
    }

    if constexpr (is_float) {
        if (operand.input_modifier.abs) {
            value = ir.FPAbs(value);
        }
        if (operand.input_modifier.neg) {
            value = ir.FPNeg(value);
        }
    } else {
        if (operand.input_modifier.abs) {
            value = ir.BitwiseAnd(value, ir.Imm32(0x7FFFFFFFu));
        }
        if (operand.input_modifier.neg) {
            value = ir.BitwiseXor(value, ir.Imm32(0x80000000u));
        }
    }
    return value;
}

template IR::U32 Translator::GetSrc<IR::U32>(const InstOperand&);
template IR::F32 Translator::GetSrc<IR::F32>(const InstOperand&);

template <typename T, bool is_signed>
T Translator::GetSrc16(const InstOperand& operand) {
    constexpr bool is_float = std::is_same_v<T, IR::F32>;

    const auto get_imm = [&](auto value) -> T {
        if constexpr (is_float) {
            return ir.Imm32(std::bit_cast<float>(value));
        } else {
            return ir.Imm32(std::bit_cast<u32>(value));
        }
    };

    const auto number_format = []() -> AmdGpu::NumberFormat {
        if constexpr (is_float) {
            return AmdGpu::NumberFormat::Float;
        } else {
            return AmdGpu::NumberFormat::Uint;
        }
    }();

    const auto bitcast_to_u = [&](auto value) -> IR::U32 {
        if constexpr (is_float) {
            return ir.BitCast<IR::U32>(value);
        } else {
            return value;
        }
    };

    const auto cast = [&](auto value) -> T {
        if constexpr (is_float) {
            return value;
        } else {
            return ir.BitFieldExtract(ir.BitCast<IR::U32>(value), ir.Imm32(0), ir.Imm32(16),
                                      is_signed);
        }
    };

    const auto op_sel = operand.op_sel.op_sel;

    T value{};
    switch (operand.field) {
    case OperandField::ScalarGPR: {
        const auto f = ir.GetScalarReg<T>(IR::ScalarReg(operand.code));
        value = cast(IR::F32{
            ir.CompositeExtract(ir.Unpack2x16(number_format, bitcast_to_u(f)), op_sel ? 1 : 0)});
        break;
    }
    case OperandField::VectorGPR: {
        const auto v = ir.GetVectorReg<T>(IR::VectorReg(operand.code));
        value = cast(IR::F32{
            ir.CompositeExtract(ir.Unpack2x16(number_format, bitcast_to_u(v)), op_sel ? 1 : 0)});
        break;
    }
    case OperandField::ConstZero:
        value = get_imm(0U);
        break;
    case OperandField::SignedConstIntPos:
        value = get_imm(operand.code - SignedConstIntPosMin + 1);
        break;
    case OperandField::SignedConstIntNeg:
        value = get_imm(-s32(operand.code) + SignedConstIntNegMin - 1);
        break;
    case OperandField::LiteralConst:
        value = get_imm(operand.code);
        break;
    case OperandField::ConstFloatPos_1_0:
        value = get_imm(1.f);
        break;
    case OperandField::ConstFloatPos_0_5:
        value = get_imm(0.5f);
        break;
    case OperandField::ConstFloatPos_2_0:
        value = get_imm(2.0f);
        break;
    case OperandField::ConstFloatPos_4_0:
        value = get_imm(4.0f);
        break;
    case OperandField::ConstFloatNeg_0_5:
        value = get_imm(-0.5f);
        break;
    case OperandField::ConstFloatNeg_1_0:
        value = get_imm(-1.0f);
        break;
    case OperandField::ConstFloatNeg_2_0:
        value = get_imm(-2.0f);
        break;
    case OperandField::ConstFloatNeg_4_0:
        value = get_imm(-4.0f);
        break;
    case OperandField::Inv2Pi:
        value = get_imm(static_cast<float>(1.0f / (2.0f * std::numbers::pi)));
        break;
    case OperandField::Sdwa:
        LOG_ERROR(Render_Recompiler, "unhandled SDWA");
        value = get_imm(0U);
        break;
    case OperandField::Dpp:
        LOG_ERROR(Render_Recompiler, "unhandled DPP");
        value = get_imm(0U);
        break;
    case OperandField::VccLo:
        if constexpr (is_float) {
            value = IR::F32{
                ir.CompositeExtract(ir.Unpack2x16(number_format, ir.GetVccLo()), op_sel ? 1 : 0)};
        } else {
            value = cast(IR::F32{ir.CompositeExtract(
                ir.Unpack2x16(number_format, bitcast_to_u(ir.GetVccLo())), op_sel ? 1 : 0)});
        }
        break;
    case OperandField::VccHi:
        UNREACHABLE();
        break;
    case OperandField::M0:
        UNREACHABLE();
        break;
    case OperandField::Scc:
        UNREACHABLE();
        break;
    default:
        UNREACHABLE_MSG("unexpected operand: {}", std::to_underlying(operand.field));
    }

    if constexpr (is_float) {
        if (operand.input_modifier.abs) {
            value = ir.FPAbs(value);
        }
        if (operand.input_modifier.neg) {
            value = ir.FPNeg(value);
        }
    } else {
        if (operand.input_modifier.abs) {
            value = ir.BitwiseAnd(value, ir.Imm32(0x7FFFFFFFu));
        }
        if (operand.input_modifier.neg) {
            value = ir.BitwiseXor(value, ir.Imm32(0x80000000u));
        }
    }
    return value;
}

template IR::U32 Translator::GetSrc16<IR::U32, false>(const InstOperand&);
template IR::U32 Translator::GetSrc16<IR::U32, true>(const InstOperand&);
template IR::F32 Translator::GetSrc16<IR::F32, false>(const InstOperand&);

IR::F32 Translator::GetSrcMix(const InstOperand& operand) {
    const auto get_imm = [&](auto value) -> IR::F32 {
        return ir.Imm32(std::bit_cast<float>(value));
    };

    const auto extract = [&](auto value) -> IR::F32 {
        const auto getter_u = [&]() {
            if constexpr (std::same_as<decltype(value), IR::ScalarReg>) {
                return ir.GetScalarReg<IR::U32>(value);
            } else {
                return ir.GetVectorReg<IR::U32>(value);
            }
        }();
        if (!operand.op_sel.op_sel_hi) {
            if constexpr (std::same_as<decltype(value), IR::ScalarReg>) {
                return ir.GetScalarReg<IR::F32>(value);
            } else {
                return ir.GetVectorReg<IR::F32>(value);
            }
        } else if (operand.op_sel.op_sel) {
            return IR::F32{
                ir.CompositeExtract(ir.Unpack2x16(AmdGpu::NumberFormat::Float, getter_u), 1)};
        } else {
            return IR::F32{
                ir.CompositeExtract(ir.Unpack2x16(AmdGpu::NumberFormat::Float, getter_u), 0)};
        }
    };

    IR::F32 value{};
    switch (operand.field) {
    case OperandField::ScalarGPR:
        value = extract(IR::ScalarReg(operand.code));
        break;
    case OperandField::VectorGPR:
        value = extract(IR::VectorReg(operand.code));
        break;
    case OperandField::ConstZero:
        value = get_imm(0U);
        break;
    case OperandField::SignedConstIntPos:
        value = get_imm(operand.code - SignedConstIntPosMin + 1);
        break;
    case OperandField::SignedConstIntNeg:
        value = get_imm(-s32(operand.code) + SignedConstIntNegMin - 1);
        break;
    case OperandField::LiteralConst:
        value = get_imm(operand.code);
        break;
    case OperandField::ConstFloatPos_1_0:
        value = get_imm(1.f);
        break;
    case OperandField::ConstFloatPos_0_5:
        value = get_imm(0.5f);
        break;
    case OperandField::ConstFloatPos_2_0:
        value = get_imm(2.0f);
        break;
    case OperandField::ConstFloatPos_4_0:
        value = get_imm(4.0f);
        break;
    case OperandField::ConstFloatNeg_0_5:
        value = get_imm(-0.5f);
        break;
    case OperandField::ConstFloatNeg_1_0:
        value = get_imm(-1.0f);
        break;
    case OperandField::ConstFloatNeg_2_0:
        value = get_imm(-2.0f);
        break;
    case OperandField::ConstFloatNeg_4_0:
        value = get_imm(-4.0f);
        break;
    case OperandField::VccLo: {
        if (!operand.op_sel.op_sel_hi) {
            value = ir.BitCast<IR::F32>(ir.GetVccLo());
        } else if (operand.op_sel.op_sel) {
            value = IR::F32{
                ir.CompositeExtract(ir.Unpack2x16(AmdGpu::NumberFormat::Float, ir.GetVccLo()), 1)};
        } else {
            value = IR::F32{
                ir.CompositeExtract(ir.Unpack2x16(AmdGpu::NumberFormat::Float, ir.GetVccLo()), 0)};
        }
        break;
    }
    case OperandField::VccHi:
        UNREACHABLE();
        break;
    case OperandField::M0:
        UNREACHABLE();
        break;
    case OperandField::Scc:
        UNREACHABLE();
        break;
    case OperandField::Inv2Pi:
        value = get_imm(static_cast<float>(1.0f / (2.0f * std::numbers::pi)));
        break;
    case OperandField::Sdwa:
        UNREACHABLE_MSG("unhandled SDWA");
        break;
    case OperandField::Dpp:
        UNREACHABLE_MSG("unhandled DPP");
        break;
    default:
        UNREACHABLE_MSG("unexpected operand: {}", std::to_underlying(operand.field));
    }

    if (operand.input_modifier.neg_hi) {
        value = ir.FPAbs(value);
    }
    if (operand.input_modifier.neg) {
        value = ir.FPNeg(value);
    }
    return value;
}

template <typename T>
T Translator::GetSrc64(const InstOperand& operand) {
    constexpr bool is_float = std::is_same_v<T, IR::F64>;

    const auto get_imm = [&](auto value) -> T {
        if constexpr (is_float) {
            return ir.Imm64(std::bit_cast<double>(value));
        } else {
            return ir.Imm64(std::bit_cast<u64>(value));
        }
    };

    T value{};
    switch (operand.field) {
    case OperandField::ScalarGPR: {
        const auto value_lo = ir.GetScalarReg(IR::ScalarReg(operand.code));
        const auto value_hi = ir.GetScalarReg(IR::ScalarReg(operand.code + 1));
        if constexpr (is_float) {
            value = ir.PackDouble2x32(ir.CompositeConstruct(value_lo, value_hi));
        } else {
            value = ir.PackUint2x32(ir.CompositeConstruct(value_lo, value_hi));
        }
        break;
    }
    case OperandField::VectorGPR: {
        const auto value_lo = ir.GetVectorReg(IR::VectorReg(operand.code));
        const auto value_hi = ir.GetVectorReg(IR::VectorReg(operand.code + 1));
        if constexpr (is_float) {
            value = ir.PackDouble2x32(ir.CompositeConstruct(value_lo, value_hi));
        } else {
            value = ir.PackUint2x32(ir.CompositeConstruct(value_lo, value_hi));
        }
        break;
    }
    case OperandField::ExecLo:
        if constexpr (is_float) {
            UNREACHABLE();
        } else {
            value = PackThreadBitMask(ir, ir.GetExec());
        }
        break;
    case OperandField::ConstZero:
        value = get_imm(0ULL);
        break;
    case OperandField::SignedConstIntPos:
        value = get_imm(s64(operand.code) - SignedConstIntPosMin + 1);
        break;
    case OperandField::SignedConstIntNeg:
        value = get_imm(-s64(operand.code) + SignedConstIntNegMin - 1);
        break;
    case OperandField::LiteralConst:
        value = get_imm(u64(operand.code));
        break;
    case OperandField::ConstFloatPos_1_0:
        value = get_imm(1.0);
        break;
    case OperandField::ConstFloatPos_0_5:
        value = get_imm(0.5);
        break;
    case OperandField::ConstFloatPos_2_0:
        value = get_imm(2.0);
        break;
    case OperandField::ConstFloatPos_4_0:
        value = get_imm(4.0);
        break;
    case OperandField::ConstFloatNeg_0_5:
        value = get_imm(-0.5);
        break;
    case OperandField::ConstFloatNeg_1_0:
        value = get_imm(-1.0);
        break;
    case OperandField::ConstFloatNeg_2_0:
        value = get_imm(-2.0);
        break;
    case OperandField::ConstFloatNeg_4_0:
        value = get_imm(-4.0);
        break;
    case OperandField::VccLo:
        if constexpr (is_float) {
            value = ir.PackDouble2x32(ir.CompositeConstruct(ir.GetVccLo(), ir.GetVccHi()));
        } else {
            value = ir.PackUint2x32(ir.CompositeConstruct(ir.GetVccLo(), ir.GetVccHi()));
        }
        break;
    case OperandField::VccHi:
    default:
        UNREACHABLE();
    }

    if constexpr (is_float) {
        if (operand.input_modifier.abs) {
            value = ir.FPAbs(value);
        }
        if (operand.input_modifier.neg) {
            value = ir.FPNeg(value);
        }
    } else {
        // GCN VOP3 abs/neg modifier bits operate on the sign bit (bit 63 for
        // 64-bit values). Unpack, modify the high dword's bit 31, repack.
        if (operand.input_modifier.abs) {
            const auto unpacked = ir.UnpackUint2x32(value);
            const auto lo = IR::U32{ir.CompositeExtract(unpacked, 0)};
            const auto hi = IR::U32{ir.CompositeExtract(unpacked, 1)};
            const auto hi_abs = ir.BitwiseAnd(hi, ir.Imm32(0x7FFFFFFFu));
            value = ir.PackUint2x32(ir.CompositeConstruct(lo, hi_abs));
        }
        if (operand.input_modifier.neg) {
            const auto unpacked = ir.UnpackUint2x32(value);
            const auto lo = IR::U32{ir.CompositeExtract(unpacked, 0)};
            const auto hi = IR::U32{ir.CompositeExtract(unpacked, 1)};
            const auto hi_neg = ir.BitwiseXor(hi, ir.Imm32(0x80000000u));
            value = ir.PackUint2x32(ir.CompositeConstruct(lo, hi_neg));
        }
    }
    return value;
}

template IR::U64 Translator::GetSrc64<IR::U64>(const InstOperand&);
template IR::F64 Translator::GetSrc64<IR::F64>(const InstOperand&);

template <typename T, bool is_signed>
pk_type<T> Translator::GetSrcPk(const InstOperand& operand) {
    constexpr bool is_float = std::is_same_v<T, IR::F32>;

    const auto get_imm = [&](auto value) -> pk_type<T> {
        if constexpr (is_float) {
            auto imm = ir.Imm32(std::bit_cast<float>(value));
            return {operand.op_sel.op_sel ? ir.Imm32(0.f) : imm,
                    operand.op_sel.op_sel_hi ? ir.Imm32(0.f) : imm};
        } else {
            auto imm = ir.Imm32(std::bit_cast<u32>(value));
            return {operand.op_sel.op_sel ? ir.Imm32(0U) : imm,
                    operand.op_sel.op_sel_hi ? ir.Imm32(0U) : imm};
        }
    };

    constexpr auto number_format = [&]() {
        if constexpr (is_float) {
            return AmdGpu::NumberFormat::Float;
        } else {
            return AmdGpu::NumberFormat::Uint;
        }
    }();

    const auto cast = [&](auto value) -> T {
        if constexpr (is_float) {
            return value;
        } else {
            return ir.BitFieldExtract(ir.BitCast<IR::U32>(value), ir.Imm32(0), ir.Imm32(16),
                                      is_signed);
        }
    };

    const auto extract = [&](auto value) -> pk_type<T> {
        auto v_unpacked = ir.Unpack2x16(number_format, value);
        return {cast(IR::F32{ir.CompositeExtract(v_unpacked, operand.op_sel.op_sel)}),
                cast(IR::F32{ir.CompositeExtract(v_unpacked, operand.op_sel.op_sel_hi)})};
    };

    pk_type<T> value{};
    switch (operand.field) {
    case OperandField::ScalarGPR: {
        value = extract(ir.GetScalarReg<IR::U32>(IR::ScalarReg(operand.code)));
        break;
    }
    case OperandField::VectorGPR: {
        value = extract(ir.GetVectorReg<IR::U32>(IR::VectorReg(operand.code)));
        break;
    }
    case OperandField::ConstZero: {
        value = get_imm(0U);
        break;
    }
    case OperandField::SignedConstIntPos: {
        value = get_imm(operand.code - SignedConstIntPosMin + 1);
        break;
    }
    case OperandField::SignedConstIntNeg: {
        value = get_imm(-s32(operand.code) + SignedConstIntNegMin - 1);
        break;
    }
    case OperandField::LiteralConst: {
        value = get_imm(operand.code);
        break;
    }
    case OperandField::ConstFloatPos_1_0: {
        value = get_imm(1.f);
        break;
    }
    case OperandField::ConstFloatPos_0_5: {
        value = get_imm(0.5f);
        break;
    }
    case OperandField::ConstFloatPos_2_0: {
        value = get_imm(2.0f);
        break;
    }
    case OperandField::ConstFloatPos_4_0: {
        value = get_imm(4.0f);
        break;
    }
    case OperandField::ConstFloatNeg_0_5: {
        value = get_imm(-0.5f);
        break;
    }
    case OperandField::ConstFloatNeg_1_0: {
        value = get_imm(-1.0f);
        break;
    }
    case OperandField::ConstFloatNeg_2_0: {
        value = get_imm(-2.0f);
        break;
    }
    case OperandField::ConstFloatNeg_4_0: {
        value = get_imm(-4.0f);
        break;
    }
    case OperandField::Inv2Pi: {
        value = get_imm(1.0f / (2.0f * std::numbers::pi_v<float>));
        break;
    }
    case OperandField::VccLo:
        value = extract(ir.GetVccLo());
        break;
    default:
        UNREACHABLE_MSG("unexpected operand: {}", std::to_underlying(operand.field));
    }

    if constexpr (is_float) {
        if (operand.input_modifier.neg) {
            value.first = ir.FPNeg(value.first);
        }
        if (operand.input_modifier.neg_hi) {
            value.second = ir.FPNeg(value.second);
        }
    } else {
        if (operand.input_modifier.neg) {
            value.first = ir.INeg(value.first);
        }
        if (operand.input_modifier.neg_hi) {
            value.second = ir.INeg(value.second);
        }
    }
    return value;
}

template pk_type<IR::U32> Translator::GetSrcPk<IR::U32, true>(const InstOperand&);
template pk_type<IR::U32> Translator::GetSrcPk<IR::U32, false>(const InstOperand&);
template pk_type<IR::F32> Translator::GetSrcPk<IR::F32, false>(const InstOperand&);

void Translator::SetDst1(const InstOperand& operand, const IR::U1& value) {
    switch (operand.field) {
    case OperandField::VccLo: {
        ir.SetVcc(value);
        const auto [lo, hi] = ThreadBitMaskDwords(ir, value);
        ir.SetVccLo(lo);
        ir.SetVccHi(hi);
        break;
    }
    case OperandField::ScalarGPR: {
        const auto reg = IR::ScalarReg(operand.code);
        ir.SetThreadBitScalarReg(reg, value);
        const auto [lo, hi] = ThreadBitMaskDwords(ir, value);
        ir.SetScalarReg(reg, lo);
        ir.SetScalarReg(reg + 1, hi);
        break;
    }
    case OperandField::ExecLo:
        ir.SetExec(value);
        break;
    default:
        UNREACHABLE_MSG("Unknown field {}", u32(operand.field));
    }
}

void Translator::SetDst(const InstOperand& operand, const IR::U32F32& value) {
    IR::U32F32 result = value;
    if (value.Type() == IR::Type::F32) {
        if (operand.output_modifier.multiplier != 0.f) {
            result = ir.FPMul(result, ir.Imm32(operand.output_modifier.multiplier));
        }
        if (operand.output_modifier.clamp) {
            result = ir.FPSaturate(result);
        }
    }

    switch (operand.field) {
    case OperandField::ScalarGPR:
        return ir.SetScalarReg(IR::ScalarReg(operand.code), result);
    case OperandField::VectorGPR:
        return ir.SetVectorReg(IR::VectorReg(operand.code), result);
    case OperandField::VccLo:
        return ir.SetVccLo(result);
    case OperandField::VccHi:
        return ir.SetVccHi(result);
    case OperandField::M0:
        return ir.SetM0(result);
    default:
        UNREACHABLE_MSG("Unknown field {}", u32(operand.field));
    }
}

template <bool is_signed>
void Translator::SetDst16(const InstOperand& operand, const IR::U32F32& value) {
    IR::U32F32 result = value;
    if (value.Type() == IR::Type::F32) {
        if (operand.output_modifier.multiplier != 0.f) {
            result = ir.FPMul(result, ir.Imm32(operand.output_modifier.multiplier));
        }
        if (operand.output_modifier.clamp) {
            result = ir.FPSaturate(result);
        }
    } else {
        if (operand.output_modifier.clamp) {
            if constexpr (is_signed) {
                result = ir.SClamp(result, ir.Imm32(-32768), ir.Imm32(32767));
            } else {
                result = ir.UMin(result, ir.Imm32(0xFFFF));
            }
        }
    }

    const auto cast = [&](auto value) -> IR::U32 {
        if (value.Type() == IR::Type::F32) {
            return ir.UConvert(32, ir.BitCast<IR::U16>(IR::F16{ir.FPConvert(16, value)}));
        } else if (value.Type() == IR::Type::U32) {
            return value;
        } else {
            UNREACHABLE();
        }
    };

    const auto op_sel = operand.op_sel.op_sel;

    switch (operand.field) {
    case OperandField::ScalarGPR: {
        const auto prev_dst = ir.GetScalarReg<IR::U32>(IR::ScalarReg(operand.code));
        const auto result_16 = cast(result);
        const auto new_dst =
            ir.BitFieldInsert(prev_dst, result_16, ir.Imm32(op_sel ? 16 : 0), ir.Imm32(16));
        return ir.SetScalarReg(IR::ScalarReg(operand.code), new_dst);
    }
    case OperandField::VectorGPR: {
        const auto prev_dst = ir.GetVectorReg<IR::U32>(IR::VectorReg(operand.code));
        const auto result_16 = cast(result);
        const auto new_dst =
            ir.BitFieldInsert(prev_dst, result_16, ir.Imm32(op_sel ? 16 : 0), ir.Imm32(16));
        return ir.SetVectorReg(IR::VectorReg(operand.code), new_dst);
    }
    case OperandField::VccLo:
        UNREACHABLE();
    case OperandField::VccHi:
        UNREACHABLE();
    case OperandField::M0:
        UNREACHABLE();
    default:
        UNREACHABLE();
    }
}

template void Translator::SetDst16<false>(const InstOperand&, const IR::U32F32& value);
template void Translator::SetDst16<true>(const InstOperand&, const IR::U32F32& value);

void Translator::SetDst64(const InstOperand& operand, const IR::U64F64& value_raw) {
    IR::U64F64 value_untyped = value_raw;

    const bool is_float = value_raw.Type() == IR::Type::F64 || value_raw.Type() == IR::Type::F32;
    if (is_float) {
        if (operand.output_modifier.multiplier != 0.f) {
            value_untyped =
                ir.FPMul(value_untyped, ir.Imm64(f64(operand.output_modifier.multiplier)));
        }
        if (operand.output_modifier.clamp) {
            value_untyped = ir.FPSaturate(value_untyped);
        }
    }

    const IR::Value unpacked{is_float ? ir.UnpackDouble2x32(IR::F64{value_untyped})
                                      : ir.UnpackUint2x32(IR::U64{value_untyped})};
    const IR::U32 lo{ir.CompositeExtract(unpacked, 0U)};
    const IR::U32 hi{ir.CompositeExtract(unpacked, 1U)};
    switch (operand.field) {
    case OperandField::ScalarGPR:
        ir.SetScalarReg(IR::ScalarReg(operand.code + 1), hi);
        return ir.SetScalarReg(IR::ScalarReg(operand.code), lo);
    case OperandField::VectorGPR:
        ir.SetVectorReg(IR::VectorReg(operand.code + 1), hi);
        return ir.SetVectorReg(IR::VectorReg(operand.code), lo);
    case OperandField::VccLo:
        ir.SetVccLo(lo);
        return ir.SetVccHi(hi);
    case OperandField::VccHi:
        UNREACHABLE();
    case OperandField::M0:
        break;
    default:
        UNREACHABLE();
    }
}

template <typename T, bool is_signed>
void Translator::SetDstPk(const InstOperand& operand, const pk_type<T>& value) {
    pk_type<T> v = value;

    if constexpr (std::is_same_v<T, IR::F32>) {
        if (operand.output_modifier.clamp) {
            v = {ir.FPSaturate(v.first), ir.FPSaturate(v.second)};
        }
    } else {
        if (operand.output_modifier.clamp) {
            if constexpr (is_signed) {
                auto lower = ir.Imm32(-32768);
                auto upper = ir.Imm32(32767);
                v = {ir.SClamp(v.first, lower, upper), ir.SClamp(v.second, lower, upper)};
            } else {
                auto imm = ir.Imm32(0xFFFF);
                v = {ir.UMin(v.first, imm), ir.UMin(v.second, imm)};
            }
        }
    }

    IR::U32 value_raw{};
    if constexpr (std::is_same_v<T, IR::F32>) {
        value_raw =
            ir.Pack2x16(AmdGpu::NumberFormat::Float, ir.CompositeConstruct(v.first, v.second));
    } else {
        value_raw = ir.Pack2x16(AmdGpu::NumberFormat::Uint,
                                ir.CompositeConstruct(ir.BitCast<IR::F32, IR::U32>(v.first),
                                                      ir.BitCast<IR::F32, IR::U32>(v.second)));
    }
    SetDst(operand, value_raw);
}

template void Translator::SetDstPk<IR::U32, false>(const InstOperand& operand,
                                                   const pk_type<IR::U32>& value);
template void Translator::SetDstPk<IR::U32, true>(const InstOperand& operand,
                                                  const pk_type<IR::U32>& value);
template void Translator::SetDstPk<IR::F32, false>(const InstOperand& operand,
                                                   const pk_type<IR::F32>& value);

void Translator::EmitFetch(const GcnInst& inst) {
    const auto code_sgpr_base = inst.src[0].code;

#if 0
    // Translate fetch shader inline using regular buffer bindings; useful for debugging.
    const auto* code = GetFetchShaderCode(info, code_sgpr_base);
    GcnCodeSlice slice(code, code + std::numeric_limits<u32>::max());
    GcnDecodeContext decoder;

    // Decode and save instructions
    while (!slice.atEnd()) {
        const auto sub_inst = decoder.decodeInstruction(slice);
        if (sub_inst.opcode == Opcode::S_SETPC_B64) {
            // Assume we're swapping back to the main shader.
            break;
        }
        TranslateInstruction(sub_inst);
    }
    return;
#endif

    info.has_fetch_shader = true;
    info.fetch_shader_sgpr_base = code_sgpr_base;

    const auto fetch_data = ParseFetchShader(info);
    ASSERT(fetch_data.has_value());

    if (EmulatorSettings.IsDumpShaders()) {
        using namespace Common::FS;
        const auto dump_dir = GetUserPath(PathType::ShaderDir) / "dumps";
        if (!std::filesystem::exists(dump_dir)) {
            std::filesystem::create_directories(dump_dir);
        }
        const auto filename = fmt::format("vs_{:#018x}.fetch.bin", info.pgm_hash);
        const auto file = IOFile{dump_dir / filename, FileAccessMode::Create};
        const auto* code = GetFetchShaderCode(info, code_sgpr_base);
        file.WriteRaw<u8>(code, fetch_data->size);
    }

    for (const auto& attrib : fetch_data->attributes) {
        const IR::Attribute attr{IR::Attribute::Param0 + attrib.semantic};
        IR::VectorReg dst_reg{attrib.dest_vgpr};

        // Read the V# of the attribute to figure out component number and type.
        const auto buffer = attrib.GetSharp(info);
        const auto values =
            ir.CompositeConstruct(ir.GetAttribute(attr, 0), ir.GetAttribute(attr, 1),
                                  ir.GetAttribute(attr, 2), ir.GetAttribute(attr, 3));
        const auto converted =
            IR::ApplyReadNumberConversionVec4(ir, values, buffer.GetNumberConversion());
        const auto swizzled = ApplySwizzle(ir, converted, buffer.DstSelect());
        for (u32 i = 0; i < 4; i++) {
            ir.SetVectorReg(dst_reg++, IR::F32{ir.CompositeExtract(swizzled, i)});
        }
    }
}

void Translator::LogMissingOpcode(const GcnInst& inst) {
    LOG_ERROR(Render_Recompiler, "Unknown opcode {} ({}, category = {})",
              magic_enum::enum_name(inst.opcode), u32(inst.opcode),
              magic_enum::enum_name(inst.category));
    info.translation_failed = true;
}

void Translator::Translate(IR::Block* block, u32 start_pc, std::span<const GcnInst> inst_list) {
    if (inst_list.empty()) {
        return;
    }
    ir = IR::IREmitter{*block, block->begin()};
    pc = start_pc;
    const bool gather_stage_trace =
        info.pgm_hash == DreamsCompat::GatherVoxelsShader &&
        std::getenv("SHADPS4_DREAMS_GATHER_STAGE_TRACE") != nullptr;
    const char* model_record_trace = std::getenv("SHADPS4_DREAMS_MODEL_RECORD_TRACE");
    const bool model_finalize_gate_trace =
        info.pgm_hash == DreamsCompat::ModelFinalizeShader && model_record_trace != nullptr &&
        std::string_view{model_record_trace} == "1";
    const bool model_producer_trace =
        (info.pgm_hash == DreamsCompat::SceneCompactShader ||
         info.pgm_hash == DreamsCompat::ModelBucketPostShader) &&
        model_record_trace != nullptr && std::string_view{model_record_trace} == "1";
    const auto emit_gather_stage = [&](u32 stage, const IR::U1& condition) {
        // Every Gather workgroup is one guest wave. Elect its first currently-active lane and
        // make only that lane contribute to the aggregate stage counter.
        const IR::U1 participating = ir.LogicalAnd(ir.GetExec(), condition);
        const IR::U32 first_lane = ir.BallotFindLsb(ir.Ballot(participating));
        const IR::U1 elected =
            ir.LogicalAnd(participating, ir.IEqual(ir.LaneId(), first_lane));
        const IR::U32 increment{ir.Select(elected, ir.Imm32(1), ir.Imm32(0))};
        const IR::U32 byte_address = ir.Imm32(
            (DreamsCompat::GatherStageTraceBaseDword + stage) * u32{sizeof(u32)});
        (void)ir.SharedAtomicIAdd(byte_address, increment, true);
    };
    const auto emit_gather_numeric = [&](u32 slot, const IR::U32& value,
                                         bool require_lane_zero = false) {
        const IR::U1 workgroup_zero = ir.LogicalAnd(
            ir.IEqual(ir.GetAttributeU32(IR::Attribute::WorkgroupId, 0), ir.Imm32(0)),
            ir.LogicalAnd(
                ir.IEqual(ir.GetAttributeU32(IR::Attribute::WorkgroupId, 1), ir.Imm32(0)),
                ir.IEqual(ir.GetAttributeU32(IR::Attribute::WorkgroupId, 2), ir.Imm32(0))));
        IR::U1 participating = ir.LogicalAnd(ir.GetExec(), workgroup_zero);
        if (require_lane_zero) {
            participating =
                ir.LogicalAnd(participating, ir.IEqual(ir.LaneId(), ir.Imm32(0)));
        }
        const IR::U32 first_lane = ir.BallotFindLsb(ir.Ballot(participating));
        const IR::U1 elected =
            ir.LogicalAnd(participating, ir.IEqual(ir.LaneId(), first_lane));
        const IR::U32 sample{ir.Select(elected, value, ir.Imm32(0))};
        const IR::U32 byte_address = ir.Imm32(
            (DreamsCompat::GatherNumericTraceBaseDword + slot) * u32{sizeof(u32)});
        (void)ir.SharedAtomicOr(byte_address, sample, true);
    };
    const auto emit_gather_candidate_head = [&](const IR::U32& value) {
        const IR::U32 workgroup_x =
            ir.GetAttributeU32(IR::Attribute::WorkgroupId, 0);
        const IR::U1 first_eight = ir.LogicalAnd(
            ir.ILessThan(workgroup_x, ir.Imm32(8), false),
            ir.LogicalAnd(
                ir.IEqual(ir.GetAttributeU32(IR::Attribute::WorkgroupId, 1), ir.Imm32(0)),
                ir.IEqual(ir.GetAttributeU32(IR::Attribute::WorkgroupId, 2), ir.Imm32(0))));
        const IR::U1 participating = ir.LogicalAnd(ir.GetExec(), first_eight);
        const IR::U32 first_lane = ir.BallotFindLsb(ir.Ballot(participating));
        const IR::U1 elected =
            ir.LogicalAnd(participating, ir.IEqual(ir.LaneId(), first_lane));
        const IR::U32 safe_workgroup_x{
            ir.Select(first_eight, workgroup_x, ir.Imm32(0))};
        const IR::U32 sample{ir.Select(elected, value, ir.Imm32(0))};
        const IR::U32 byte_address = ir.IAdd(
            ir.Imm32((DreamsCompat::GatherNumericTraceBaseDword + 37) *
                     u32{sizeof(u32)}),
            ir.ShiftLeftLogical(safe_workgroup_x, ir.Imm32(2)));
        (void)ir.SharedAtomicOr(byte_address, sample, true);

        const IR::U32 validity_bit =
            ir.ShiftLeftLogical(ir.Imm32(1), safe_workgroup_x);
        const IR::U32 validity_sample{
            ir.Select(elected, validity_bit, ir.Imm32(0))};
        const IR::U32 validity_address = ir.Imm32(
            (DreamsCompat::GatherNumericTraceBaseDword + 45) * u32{sizeof(u32)});
        (void)ir.SharedAtomicOr(validity_address, validity_sample, true);
    };
    const auto emit_model_finalize_gate_counter = [&](u32 slot, const IR::U1& condition) {
        const IR::U1 participating = ir.LogicalAnd(ir.GetExec(), condition);
        const IR::U32 increment{ir.Select(participating, ir.Imm32(1), ir.Imm32(0))};
        const IR::U32 byte_address = ir.Imm32(
            (DreamsCompat::ModelFinalizeGateTraceBaseDword + slot) * u32{sizeof(u32)});
        (void)ir.SharedAtomicIAdd(byte_address, increment, true);
    };
    const auto emit_model_finalize_gate_numeric = [&](u32 slot, const IR::U32& value) {
        const IR::U32 workgroup_x =
            ir.GetAttributeU32(IR::Attribute::WorkgroupId, 0);
        const IR::U1 sampled_workgroup = ir.LogicalAnd(
            ir.ILessThan(workgroup_x,
                         ir.Imm32(DreamsCompat::ModelFinalizeGateNumericWorkgroups), false),
            ir.LogicalAnd(
                ir.IEqual(ir.GetAttributeU32(IR::Attribute::WorkgroupId, 1), ir.Imm32(0)),
                ir.IEqual(ir.GetAttributeU32(IR::Attribute::WorkgroupId, 2), ir.Imm32(0))));
        const IR::U1 participating = ir.LogicalAnd(ir.GetExec(), sampled_workgroup);
        const IR::U32 first_lane = ir.BallotFindLsb(ir.Ballot(participating));
        const IR::U1 elected =
            ir.LogicalAnd(participating, ir.IEqual(ir.LaneId(), first_lane));
        const IR::U32 safe_workgroup_x{
            ir.Select(sampled_workgroup, workgroup_x, ir.Imm32(0))};
        const IR::U32 sample{ir.Select(elected, value, ir.Imm32(0))};
        const IR::U32 dword_offset = ir.IAdd(
            ir.IMul(safe_workgroup_x,
                    ir.Imm32(DreamsCompat::ModelFinalizeGateNumericStride)),
            ir.Imm32(slot));
        const IR::U32 byte_address = ir.IAdd(
            ir.Imm32(DreamsCompat::ModelFinalizeGateNumericBaseDword * u32{sizeof(u32)}),
            ir.ShiftLeftLogical(dword_offset, ir.Imm32(2)));
        (void)ir.SharedAtomicOr(byte_address, sample, true);
    };
    const auto emit_model_producer_counter = [&](u32 slot, const IR::U1& condition) {
        const IR::U1 participating = ir.LogicalAnd(ir.GetExec(), condition);
        const IR::U32 increment{ir.Select(participating, ir.Imm32(1), ir.Imm32(0))};
        const IR::U32 address = ir.Imm32(
            (DreamsCompat::ModelProducerTraceBaseDword + slot) * u32{sizeof(u32)});
        (void)ir.SharedAtomicIAdd(address, increment, true);
    };
    const auto emit_model_producer_sum = [&](u32 slot, const IR::U32& value,
                                              const IR::U1& condition) {
        const IR::U1 participating = ir.LogicalAnd(ir.GetExec(), condition);
        const IR::U32 contribution{ir.Select(participating, value, ir.Imm32(0))};
        const IR::U32 address = ir.Imm32(
            (DreamsCompat::ModelProducerTraceBaseDword + slot) * u32{sizeof(u32)});
        (void)ir.SharedAtomicIAdd(address, contribution, true);
    };
    const auto emit_model_producer_min = [&](u32 slot, const IR::U32& value) {
        const IR::U32 contribution{ir.Select(ir.GetExec(), value, ir.Imm32(0xffffffff))};
        const IR::U32 address = ir.Imm32(
            (DreamsCompat::ModelProducerTraceBaseDword + slot) * u32{sizeof(u32)});
        (void)ir.SharedAtomicIMin(address, contribution, false, true);
    };
    const auto emit_model_producer_max = [&](u32 slot, const IR::U32& value) {
        const IR::U32 contribution{ir.Select(ir.GetExec(), value, ir.Imm32(0))};
        const IR::U32 address = ir.Imm32(
            (DreamsCompat::ModelProducerTraceBaseDword + slot) * u32{sizeof(u32)});
        (void)ir.SharedAtomicIMax(address, contribution, false, true);
    };
    const auto emit_model_producer_numeric = [&](const IR::U32& dword_offset,
                                                  const IR::U32& value,
                                                  const IR::U1& condition) {
        const IR::U1 participating = ir.LogicalAnd(ir.GetExec(), condition);
        const IR::U32 safe_offset{ir.Select(participating, dword_offset, ir.Imm32(0))};
        const IR::U32 sample{ir.Select(participating, value, ir.Imm32(0))};
        const IR::U32 byte_address = ir.IAdd(
            ir.Imm32(DreamsCompat::ModelProducerNumericBaseDword * u32{sizeof(u32)}),
            ir.ShiftLeftLogical(safe_offset, ir.Imm32(2)));
        (void)ir.SharedAtomicOr(byte_address, sample, true);
    };
    const auto emit_model_compact_wave_sample = [&](u32 slot, const IR::U32& value) {
        const IR::U32 wave = ir.GetScalarReg<IR::U32>(IR::ScalarReg::S33);
        const IR::U1 sampled_wave = ir.ILessThan(wave, ir.Imm32(8), false);
        const IR::U1 participating = ir.LogicalAnd(ir.GetExec(), sampled_wave);
        const IR::U32 first_lane = ir.BallotFindLsb(ir.Ballot(participating));
        const IR::U1 elected =
            ir.LogicalAnd(participating, ir.IEqual(ir.LaneId(), first_lane));
        const IR::U32 safe_wave{ir.Select(sampled_wave, wave, ir.Imm32(0))};
        const IR::U32 offset = ir.IAdd(ir.IMul(safe_wave, ir.Imm32(8)), ir.Imm32(32 + slot));
        emit_model_producer_numeric(offset, value, elected);
    };
    const auto emit_model_compact_store = [&](const IR::U32& index, const IR::U32& handle) {
        const IR::U1 participating = ir.GetExec();
        const IR::U32 increment{ir.Select(participating, ir.Imm32(1), ir.Imm32(0))};
        const IR::U32 count_address = ir.Imm32(
            (DreamsCompat::ModelProducerTraceBaseDword + 6) * u32{sizeof(u32)});
        const IR::U32 ticket{ir.SharedAtomicIAdd(count_address, increment, true)};
        emit_model_producer_min(9, index);
        emit_model_producer_max(10, index);
        const IR::U1 sampled = ir.LogicalAnd(participating,
                                             ir.ILessThan(ticket, ir.Imm32(16), false));
        const IR::U32 safe_ticket{ir.Select(sampled, ticket, ir.Imm32(0))};
        const IR::U32 pair_offset = ir.ShiftLeftLogical(safe_ticket, ir.Imm32(1));
        emit_model_producer_numeric(pair_offset, index, sampled);
        emit_model_producer_numeric(ir.IAdd(pair_offset, ir.Imm32(1)), handle, sampled);
    };
    const auto emit_model_post_sample = [&](u32 slot, const IR::U32& value) {
        const IR::U32 local = ir.GetAttributeU32(IR::Attribute::LocalInvocationId, 0);
        const IR::U1 sampled = ir.ILessThan(local, ir.Imm32(16), false);
        const IR::U32 safe_local{ir.Select(sampled, local, ir.Imm32(0))};
        const IR::U32 offset =
            ir.IAdd(ir.IMul(safe_local, ir.Imm32(4)), ir.Imm32(96 + slot));
        emit_model_producer_numeric(offset, value, sampled);
    };
    for (const auto& inst : inst_list) {
        const u32 inst_pc = pc;
        pc += inst.length;

        if (gather_stage_trace) {
            if (inst_pc == 0x180) {
                // Snapshot the lane-zero candidate coordinates immediately before the 1023 gate.
                emit_gather_numeric(3, ir.Imm32(1), true);
                emit_gather_numeric(4, ir.LaneId(), true);
                emit_gather_numeric(
                    5, ir.GetVectorReg<IR::U32>(IR::VectorReg::V8), true);
                emit_gather_numeric(
                    6, ir.GetVectorReg<IR::U32>(IR::VectorReg::V11), true);
                emit_gather_numeric(
                    7, ir.GetVectorReg<IR::U32>(IR::VectorReg::V15), true);
            } else if (inst_pc == 0x410) {
                // The first surviving lane's shifted absolute coordinates, before subtraction.
                emit_gather_numeric(18, ir.Imm32(1));
                emit_gather_numeric(19, ir.LaneId());
                emit_gather_numeric(
                    20, ir.GetVectorReg<IR::U32>(IR::VectorReg::V1));
                emit_gather_numeric(
                    21, ir.GetVectorReg<IR::U32>(IR::VectorReg::V2));
                emit_gather_numeric(
                    22, ir.GetVectorReg<IR::U32>(IR::VectorReg::V3));
            } else if (inst_pc == 0x2f4) {
                emit_gather_numeric(34, ir.Imm32(1));
                emit_gather_numeric(35, ir.LaneId());
                emit_gather_numeric(
                    36, ir.GetVectorReg<IR::U32>(IR::VectorReg::V18));
            } else if (inst_pc == 0x424) {
                const IR::U32 v4 = ir.GetVectorReg<IR::U32>(IR::VectorReg::V4);
                const IR::U32 v11 = ir.GetVectorReg<IR::U32>(IR::VectorReg::V11);
                const IR::U32 v5 = ir.GetVectorReg<IR::U32>(IR::VectorReg::V5);
                const IR::U32 maximum =
                    ir.GetVectorReg<IR::U32>(IR::VectorReg::V1);
                emit_gather_stage(66, ir.IGreaterThanEqual(v4, ir.Imm32(2048), false));
                emit_gather_stage(67, ir.IGreaterThanEqual(v11, ir.Imm32(2048), false));
                emit_gather_stage(68, ir.IGreaterThanEqual(v5, ir.Imm32(2048), false));
                emit_gather_stage(
                    69, ir.INotEqual(ir.BitwiseAnd(maximum, ir.Imm32(0x80000000u)),
                                     ir.Imm32(0)));
                emit_gather_stage(70, ir.IEqual(maximum, ir.Imm32(0xffffffffu)));
            } else if (inst_pc == 0xb30) {
                const IR::F32 produced_value =
                    ir.GetVectorReg<IR::F32>(IR::VectorReg::V1);
                const IR::F32 exact_one{ir.Imm32(1.0f)};
                const IR::U1 exact_one_lane{ir.FPEqual(produced_value, exact_one)};
                const IR::U1 any_active_not_exact_one = ir.GroupAny(
                    ir.LogicalAnd(ir.GetExec(), ir.LogicalNot(exact_one_lane)));
                const IR::U1 wave_all_exact_one{ir.LogicalNot(any_active_not_exact_one)};
                emit_gather_stage(26, ir.Imm1(true));
                emit_gather_stage(27, ir.FPIsNan(produced_value));
                emit_gather_stage(28, wave_all_exact_one);
                emit_gather_stage(29, ir.FPNotEqual(produced_value, exact_one));
                emit_gather_stage(30, ir.FPLessThan(produced_value, ir.Imm32(0.875f)));
            } else if (inst_pc == 0x67c) {
                const IR::U32 explicit_mask = ir.BitwiseOr(
                    ir.GetVectorReg<IR::U32>(IR::VectorReg::V2),
                    ir.GetVectorReg<IR::U32>(IR::VectorReg::V3));
                const IR::U32 fallback_mask = ir.BitwiseOr(
                    ir.GetVectorReg<IR::U32>(IR::VectorReg::V4),
                    ir.GetVectorReg<IR::U32>(IR::VectorReg::V5));
                emit_gather_stage(47, ir.INotEqual(explicit_mask, ir.Imm32(0)));
                emit_gather_stage(48, ir.INotEqual(fallback_mask, ir.Imm32(0)));
            } else if (inst_pc == 0x1630) {
                const IR::U32 v47 = ir.GetVectorReg<IR::U32>(IR::VectorReg::V47);
                emit_gather_stage(13, ir.Imm1(true)); // classification reduction reached
                emit_gather_stage(14, ir.INotEqual(v47, ir.Imm32(0)));
                emit_gather_stage(
                    15, ir.INotEqual(ir.BitwiseAnd(v47, ir.Imm32(1u << 6)), ir.Imm32(0)));
                emit_gather_stage(
                    16, ir.INotEqual(ir.BitwiseAnd(v47, ir.Imm32(1u << 7)), ir.Imm32(0)));
            } else if (inst_pc == 0x17d8) {
                emit_gather_stage(1, ir.Imm1(true)); // active before classification reduction
            } else if (inst_pc == 0x229c) {
                emit_gather_stage(6, ir.Imm1(true)); // reached guest GDS64 atomic
            }
        }

        if (model_finalize_gate_trace && inst_pc == 0x4d0) {
            // Direct-path candidate gate entry. Record one representative active lane from each
            // of the first eight workgroups before v7|v16 is formed at raw 0x4d0.
            emit_model_finalize_gate_counter(0, ir.Imm1(true));
            emit_model_finalize_gate_numeric(
                0, ir.GetVectorReg<IR::U32>(IR::VectorReg::V7));
            emit_model_finalize_gate_numeric(
                1, ir.GetVectorReg<IR::U32>(IR::VectorReg::V16));
            emit_model_finalize_gate_numeric(
                2, ir.GetVectorReg<IR::U32>(IR::VectorReg::V8));
            emit_model_finalize_gate_numeric(
                3, ir.GetVectorReg<IR::U32>(IR::VectorReg::V2));
            emit_model_finalize_gate_numeric(
                4, ir.GetVectorReg<IR::U32>(IR::VectorReg::V13));
            emit_model_finalize_gate_numeric(
                5, ir.GetVectorReg<IR::U32>(IR::VectorReg::V21));
            emit_model_finalize_gate_numeric(
                6, ir.GetVectorReg<IR::U32>(IR::VectorReg::V0));
            emit_model_finalize_gate_numeric(7, ir.Imm32(1));
        }

        if (model_producer_trace && info.pgm_hash == DreamsCompat::SceneCompactShader &&
            inst_pc == 0x9c) {
            // Sample the scalar entry predicate inputs before s_and_saveexec overwrites s6.
            emit_model_compact_wave_sample(
                0, ir.GetScalarReg<IR::U32>(IR::ScalarReg::S33));
            emit_model_compact_wave_sample(
                1, ir.GetScalarReg<IR::U32>(IR::ScalarReg::S6));
            emit_model_compact_wave_sample(
                2, ir.GetScalarReg<IR::U32>(IR::ScalarReg::S13));
            emit_model_compact_wave_sample(
                3, ir.GetScalarReg<IR::U32>(IR::ScalarReg::S7));
            emit_model_compact_wave_sample(
                4, ir.GetScalarReg<IR::U32>(IR::ScalarReg::S12));
        }

        // Special case for emitting fetch shader.
        if (inst.opcode == Opcode::S_SWAPPC_B64) {
            ASSERT(info.stage == Stage::Vertex || info.stage == Stage::Export ||
                   info.stage == Stage::Local);
            EmitFetch(inst);
            continue;
        }

        TranslateInstruction(inst);

        if (model_producer_trace && info.pgm_hash == DreamsCompat::SceneCompactShader) {
            switch (inst_pc) {
            case 0x9c:
                emit_model_producer_counter(0, ir.Imm1(true));
                break;
            case 0x4e54: {
                const IR::U32 classification =
                    ir.GetVectorReg<IR::U32>(IR::VectorReg::V7);
                emit_model_producer_counter(1, ir.IEqual(classification, ir.Imm32(0)));
                emit_model_producer_counter(2, ir.IEqual(classification, ir.Imm32(1)));
                emit_model_producer_counter(3, ir.IEqual(classification, ir.Imm32(2)));
                emit_model_producer_counter(4, ir.IEqual(classification, ir.Imm32(3)));
                emit_model_compact_wave_sample(5, classification);
                break;
            }
            case 0x4f68:
                emit_model_compact_wave_sample(
                    6, ir.GetVectorReg<IR::U32>(IR::VectorReg::V2));
                break;
            case 0x4fbc:
                emit_model_compact_wave_sample(
                    7, ir.GetScalarReg<IR::U32>(IR::ScalarReg::S2));
                break;
            case 0x4fa8:
                emit_model_producer_counter(5, ir.Imm1(true));
                break;
            case 0x4fc0:
                emit_model_compact_store(
                    ir.GetVectorReg<IR::U32>(IR::VectorReg::V2),
                    ir.GetVectorReg<IR::U32>(IR::VectorReg::V0));
                break;
            default:
                break;
            }
        } else if (model_producer_trace &&
                   info.pgm_hash == DreamsCompat::ModelBucketPostShader) {
            switch (inst_pc) {
            case 0x8:
                emit_model_producer_counter(16, ir.Imm1(true));
                break;
            case 0x3c: {
                const IR::U32 base = ir.GetVectorReg<IR::U32>(IR::VectorReg::V1);
                const IR::U32 end = ir.GetVectorReg<IR::U32>(IR::VectorReg::V4);
                const IR::U32 range = ir.GetVectorReg<IR::U32>(IR::VectorReg::V2);
                const IR::U1 zero_range = ir.IEqual(range, ir.Imm32(0));
                emit_model_producer_counter(17, zero_range);
                emit_model_producer_counter(18, ir.LogicalNot(zero_range));
                emit_model_producer_counter(19, ir.ILessThan(end, base, false));
                emit_model_producer_sum(20, range, ir.Imm1(true));
                emit_model_producer_min(22, base);
                emit_model_producer_max(23, base);
                emit_model_producer_min(24, end);
                emit_model_producer_max(25, end);
                emit_model_post_sample(0, base);
                emit_model_post_sample(1, end);
                emit_model_post_sample(2, range);
                emit_model_post_sample(
                    3, ir.GetScalarReg<IR::U32>(IR::ScalarReg::S2));
                break;
            }
            case 0x4c:
                emit_model_producer_counter(21, ir.Imm1(true));
                break;
            default:
                break;
            }
        }

        if (model_finalize_gate_trace) {
            switch (inst_pc) {
            case 0x4d4: {
                const IR::U1 zero_flags = GetThreadBitScalarReg(IR::ScalarReg::S8);
                emit_model_finalize_gate_counter(1, zero_flags);
                emit_model_finalize_gate_counter(9, ir.LogicalNot(zero_flags));
                break;
            }
            case 0x4e0: {
                const IR::U1 ticket_mismatch = ir.GetVcc();
                emit_model_finalize_gate_counter(2, ticket_mismatch);
                emit_model_finalize_gate_counter(8, ir.LogicalNot(ticket_mismatch));
                break;
            }
            case 0x4e4: {
                const IR::U1 float_compare = GetThreadBitScalarReg(IR::ScalarReg::S12);
                const IR::F32 value = ir.GetVectorReg<IR::F32>(IR::VectorReg::V13);
                const IR::F32 threshold = ir.GetVectorReg<IR::F32>(IR::VectorReg::V21);
                emit_model_finalize_gate_counter(3, float_compare);
                emit_model_finalize_gate_counter(7, ir.LogicalNot(float_compare));
                emit_model_finalize_gate_counter(12, ir.FPIsNan(value));
                emit_model_finalize_gate_counter(13, ir.FPIsNan(threshold));
                emit_model_finalize_gate_counter(14,
                                                  ir.FPLessThan(threshold, ir.Imm32(0.0f)));
                emit_model_finalize_gate_counter(15,
                                                  ir.FPLessThan(value, ir.Imm32(0.0f)));
                break;
            }
            case 0x4ec:
                emit_model_finalize_gate_counter(
                    4, GetThreadBitScalarReg(IR::ScalarReg::S10));
                break;
            case 0x4f8: {
                const IR::U1 or_gate = ir.GetVcc();
                emit_model_finalize_gate_counter(5, or_gate);
                emit_model_finalize_gate_counter(10, ir.LogicalNot(or_gate));
                break;
            }
            case 0x4fc: {
                const IR::U1 final_and = GetThreadBitScalarReg(IR::ScalarReg::S8);
                emit_model_finalize_gate_counter(6, final_and);
                emit_model_finalize_gate_counter(11, ir.LogicalNot(final_and));
                break;
            }
            default:
                break;
            }
        }

        if (gather_stage_trace) {
            switch (inst_pc) {
            case 0x28:
                emit_gather_numeric(0, ir.Imm32(1), true);
                emit_gather_numeric(1, ir.LaneId(), true);
                emit_gather_numeric(
                    2, ir.GetVectorReg<IR::U32>(IR::VectorReg::V9), true);
                emit_gather_candidate_head(
                    ir.GetVectorReg<IR::U32>(IR::VectorReg::V9));
                break;
            case 0x180:
                // The 0x180 comparison includes its literal dword at raw 0x184.
                emit_gather_numeric(8, ir.Imm32(1));
                emit_gather_numeric(9, ir.LaneId());
                emit_gather_numeric(
                    10, ir.GetVectorReg<IR::U32>(IR::VectorReg::V8));
                emit_gather_numeric(
                    11, ir.GetVectorReg<IR::U32>(IR::VectorReg::V11));
                emit_gather_numeric(
                    12, ir.GetVectorReg<IR::U32>(IR::VectorReg::V15));
                break;
            case 0x194:
                // S_AND_SAVEEXEC applies the 1023 gate here; record its first surviving lane.
                emit_gather_numeric(13, ir.Imm32(1));
                emit_gather_numeric(14, ir.LaneId());
                emit_gather_numeric(
                    15, ir.GetVectorReg<IR::U32>(IR::VectorReg::V8));
                emit_gather_numeric(
                    16, ir.GetVectorReg<IR::U32>(IR::VectorReg::V11));
                emit_gather_numeric(
                    17, ir.GetVectorReg<IR::U32>(IR::VectorReg::V15));
                break;
            case 0x270:
                emit_gather_numeric(28, ir.Imm32(1));
                emit_gather_numeric(29, ir.LaneId());
                emit_gather_numeric(
                    30, ir.GetVectorReg<IR::U32>(IR::VectorReg::V5));
                break;
            case 0x2d0:
                emit_gather_numeric(31, ir.Imm32(1));
                emit_gather_numeric(32, ir.LaneId());
                emit_gather_numeric(
                    33, ir.GetVectorReg<IR::U32>(IR::VectorReg::V1));
                break;
            case 0x10:
                emit_gather_stage(0, ir.Imm1(true)); // entry
                break;
            case 0x7b4: {
                const IR::U1 explicit_hit = ir.GetVcc();
                emit_gather_stage(31, explicit_hit);
                emit_gather_stage(32, ir.LogicalNot(explicit_hit));
                break;
            }
            case 0x3b0: {
                const IR::U1 direct_buffer_11 = ir.GetVcc();
                emit_gather_stage(39, direct_buffer_11);
                emit_gather_stage(40, ir.LogicalNot(direct_buffer_11));
                break;
            }
            case 0x410:
                emit_gather_stage(63, ir.GetVcc());
                break;
            case 0x414:
                emit_gather_stage(64, ir.GetVcc());
                break;
            case 0x418:
                emit_gather_numeric(23, ir.Imm32(1));
                emit_gather_numeric(24, ir.LaneId());
                emit_gather_numeric(
                    25, ir.GetVectorReg<IR::U32>(IR::VectorReg::V4));
                emit_gather_numeric(
                    26, ir.GetVectorReg<IR::U32>(IR::VectorReg::V11));
                emit_gather_numeric(
                    27, ir.GetVectorReg<IR::U32>(IR::VectorReg::V5));
                emit_gather_stage(65, ir.GetVcc());
                break;
            case 0x424: {
                const IR::U1 in_range = ir.GetVcc();
                emit_gather_stage(51, in_range);
                emit_gather_stage(52, ir.LogicalNot(in_range));
                break;
            }
            case 0x45c: {
                // The root-negative comparison is at raw 0x45c; raw 0x458 is its waitcnt.
                const IR::U1 root_negative = ir.GetVcc();
                emit_gather_stage(53, root_negative);
                emit_gather_stage(54, ir.LogicalNot(root_negative));
                break;
            }
            case 0x464: {
                const IR::U1 root_minus_two = ir.GetVcc();
                const IR::U1 root_negative =
                    GetThreadBitScalarReg(IR::ScalarReg::S2);
                emit_gather_stage(55, root_minus_two);
                emit_gather_stage(
                    56, ir.LogicalAnd(root_negative, ir.LogicalNot(root_minus_two)));
                break;
            }
            case 0x478:
                emit_gather_stage(57, ir.Imm1(true));
                break;
            case 0x2f4: {
                const IR::U1 within_probe_limit = ir.GetVcc();
                emit_gather_stage(58, ir.Imm1(true));
                emit_gather_stage(59, within_probe_limit);
                emit_gather_stage(60, ir.LogicalNot(within_probe_limit));
                break;
            }
            case 0x374: {
                // The guest comparison is loaded_key != target_key, so VCC means mismatch.
                const IR::U1 key_mismatch = ir.GetVcc();
                emit_gather_stage(61, ir.LogicalNot(key_mismatch));
                emit_gather_stage(62, key_mismatch);
                break;
            }
            case 0x554: {
                const IR::U1 membership = ir.GetVcc();
                emit_gather_stage(41, membership);
                emit_gather_stage(42, ir.LogicalNot(membership));
                break;
            }
            case 0x5c0: {
                const IR::U1 membership = ir.GetVcc();
                emit_gather_stage(43, membership);
                emit_gather_stage(44, ir.LogicalNot(membership));
                break;
            }
            case 0x620: {
                const IR::U1 membership = ir.GetVcc();
                emit_gather_stage(45, membership);
                emit_gather_stage(46, ir.LogicalNot(membership));
                break;
            }
            case 0x67c: {
                const IR::U32 explicit_mask = ir.BitwiseOr(
                    ir.GetVectorReg<IR::U32>(IR::VectorReg::V2),
                    ir.GetVectorReg<IR::U32>(IR::VectorReg::V3));
                const IR::U32 fallback_mask = ir.BitwiseOr(
                    ir.GetVectorReg<IR::U32>(IR::VectorReg::V4),
                    ir.GetVectorReg<IR::U32>(IR::VectorReg::V5));
                emit_gather_stage(49, ir.INotEqual(explicit_mask, ir.Imm32(0)));
                emit_gather_stage(50, ir.INotEqual(fallback_mask, ir.Imm32(0)));
                break;
            }
            case 0x800: {
                const IR::F32 explicit_value =
                    ir.GetVectorReg<IR::F32>(IR::VectorReg::V1);
                emit_gather_stage(33,
                                  ir.FPLessThan(explicit_value, ir.Imm32(-0.1f)));
                break;
            }
            case 0x81c: {
                const IR::U1 fallback_negative = ir.GetVcc();
                emit_gather_stage(34, fallback_negative);
                emit_gather_stage(35, ir.LogicalNot(fallback_negative));
                break;
            }
            case 0xabc: {
                const IR::F32 low_half = ir.GetVectorReg<IR::F32>(IR::VectorReg::V1);
                emit_gather_stage(36, ir.FPLessThan(low_half, ir.Imm32(-0.1f)));
                break;
            }
            case 0xac0: {
                const IR::U1 special_abs8 = ir.GetVcc();
                emit_gather_stage(37, special_abs8);
                emit_gather_stage(38, ir.LogicalNot(special_abs8));
                break;
            }
            case 0x17d8:
                emit_gather_stage(2, GetThreadBitScalarReg(IR::ScalarReg::S2)); // class any
                break;
            case 0x1638:
                emit_gather_stage(17, ir.GetVcc()); // v47 bits 6 and 7 are both set
                break;
            case 0x164c: {
                const IR::U32 v35 = ir.GetVectorReg<IR::U32>(IR::VectorReg::V35);
                emit_gather_stage(18, ir.INotEqual(v35, ir.Imm32(0)));
                break;
            }
            case 0xc68: {
                const std::array inputs{
                    ir.GetVectorReg<IR::F32>(IR::VectorReg::V1),
                    ir.GetVectorReg<IR::F32>(IR::VectorReg::V2),
                    ir.GetVectorReg<IR::F32>(IR::VectorReg::V7),
                    ir.GetVectorReg<IR::F32>(IR::VectorReg::V8),
                    ir.GetVectorReg<IR::F32>(IR::VectorReg::V5),
                    ir.GetVectorReg<IR::F32>(IR::VectorReg::V6),
                    ir.GetVectorReg<IR::F32>(IR::VectorReg::V3),
                    ir.GetVectorReg<IR::F32>(IR::VectorReg::V4),
                };
                const IR::F32 exact_one{ir.Imm32(1.0f)};
                const IR::F32 threshold{ir.Imm32(0.875f)};
                IR::U1 any_nan{ir.Imm1(false)};
                IR::U1 lane_all_exact_one{ir.Imm1(true)};
                IR::U1 any_not_exact_one{ir.Imm1(false)};
                IR::U1 any_less_threshold{ir.Imm1(false)};
                IR::U1 any_equal_threshold{ir.Imm1(false)};
                IR::U1 any_greater_one{ir.Imm1(false)};
                for (const IR::F32& input : inputs) {
                    any_nan = ir.LogicalOr(any_nan, ir.FPIsNan(input));
                    const IR::U1 exact_one_sample{ir.FPEqual(input, exact_one)};
                    lane_all_exact_one =
                        ir.LogicalAnd(lane_all_exact_one, exact_one_sample);
                    any_not_exact_one =
                        ir.LogicalOr(any_not_exact_one, ir.FPNotEqual(input, exact_one));
                    any_less_threshold =
                        ir.LogicalOr(any_less_threshold, ir.FPLessThan(input, threshold));
                    any_equal_threshold =
                        ir.LogicalOr(any_equal_threshold, ir.FPEqual(input, threshold));
                    any_greater_one =
                        ir.LogicalOr(any_greater_one, ir.FPGreaterThan(input, exact_one));
                }
                const IR::U1 any_active_not_exact_one = ir.GroupAny(
                    ir.LogicalAnd(ir.GetExec(), ir.LogicalNot(lane_all_exact_one)));
                const IR::U1 wave_all_exact_one{ir.LogicalNot(any_active_not_exact_one)};
                emit_gather_stage(19, ir.Imm1(true));
                emit_gather_stage(20, any_nan);
                emit_gather_stage(21, wave_all_exact_one);
                emit_gather_stage(22, any_not_exact_one);
                emit_gather_stage(23, any_less_threshold);
                emit_gather_stage(24, any_equal_threshold);
                emit_gather_stage(25, any_greater_one);
                break;
            }
            case 0x207c:
                emit_gather_stage(8, GetThreadBitScalarReg(IR::ScalarReg::S6)); // local id < 8
                break;
            case 0x2084:
                emit_gather_stage(9,
                                  GetThreadBitScalarReg(IR::ScalarReg::S12)); // class mask empty
                break;
            case 0x20b8:
                emit_gather_stage(10,
                                  GetThreadBitScalarReg(IR::ScalarReg::S14)); // CB93 > 0
                break;
            case 0x2110:
                emit_gather_stage(11, ir.GetVcc()); // (local id < 8) and (CB93 > 0)
                break;
            case 0x2114:
                emit_gather_stage(12, ir.GetVcc()); // final mask before saveexec
                break;
            case 0x2118:
                emit_gather_stage(3, ir.Imm1(true)); // hash attempt (new EXEC)
                break;
            case 0x2230:
                emit_gather_stage(4, ir.GetVcc()); // hash found
                break;
            case 0x224c: {
                const IR::U1 unresolved = ir.GroupAny(ir.GetVcc());
                emit_gather_stage(5, ir.LogicalNot(unresolved)); // take output branch
                emit_gather_stage(7, unresolved);                // sentinel/error branch
                break;
            }
            default:
                break;
            }
        }
    }
}

void Translator::TranslateInstruction(const GcnInst& inst) {
    // Emit instructions for each category.
    switch (inst.category) {
    case InstCategory::DataShare:
        EmitDataShare(inst);
        break;
    case InstCategory::VectorInterpolation:
        EmitVectorInterpolation(inst);
        break;
    case InstCategory::ScalarMemory:
        EmitScalarMemory(inst);
        break;
    case InstCategory::VectorMemory:
        EmitVectorMemory(inst);
        break;
    case InstCategory::Export:
        EmitExport(inst);
        break;
    case InstCategory::FlowControl:
        EmitFlowControl(inst);
        break;
    case InstCategory::ScalarALU:
        EmitScalarAlu(inst);
        break;
    case InstCategory::VectorALU:
        EmitVectorAlu(inst);
        break;
    case InstCategory::DebugProfile:
        break;
    default:
        UNREACHABLE();
    }
}

} // namespace Shader::Gcn
