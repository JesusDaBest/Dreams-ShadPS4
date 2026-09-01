// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <array>
#include "shader_recompiler/backend/spirv/emit_spirv_instructions.h"
#include "shader_recompiler/backend/spirv/spirv_emit_context.h"
#include "shader_recompiler/dreams_compat.h"
#include "shader_recompiler/ir/attribute.h"
#include "shader_recompiler/ir/debug_print.h"

namespace Shader::Backend::SPIRV {

void EmitPrologue(EmitContext& ctx) {
    if (ctx.stage == Stage::Fragment) {
        ctx.DefineAmdPerVertexAttribs();
    }
    if (ctx.info.loads.Get(IR::Attribute::WorkgroupIndex)) {
        ctx.DefineWorkgroupIndex();
    }
    ctx.DefineBufferProperties();
}

void ConvertDepthMode(EmitContext& ctx) {
    const Id type{ctx.F32[1]};
    const Id position{ctx.OpLoad(ctx.F32[4], ctx.output_position)};
    const Id z{ctx.OpCompositeExtract(type, position, 2u)};
    const Id w{ctx.OpCompositeExtract(type, position, 3u)};
    const Id screen_depth{ctx.OpFMul(type, ctx.OpFAdd(type, z, w), ctx.Constant(type, 0.5f))};
    const Id vector{ctx.OpCompositeInsert(ctx.F32[4], screen_depth, position, 2u)};
    ctx.OpStore(ctx.output_position, vector);
}

void ConvertPositionToClipSpace(EmitContext& ctx) {
    ASSERT_MSG(!ctx.info.stores.GetAny(IR::Attribute::ViewportIndex),
               "Multi-viewport with shader clip space conversion not yet implemented.");

    const Id type{ctx.F32[1]};
    Id position{ctx.OpLoad(ctx.F32[4], ctx.output_position)};
    const Id x{ctx.OpCompositeExtract(type, position, 0u)};
    const Id y{ctx.OpCompositeExtract(type, position, 1u)};
    const Id z{ctx.OpCompositeExtract(type, position, 2u)};
    const Id w{ctx.OpCompositeExtract(type, position, 3u)};
    const Id xoffset_ptr{ctx.OpAccessChain(ctx.TypePointer(spv::StorageClass::PushConstant, type),
                                           ctx.push_data_block,
                                           ctx.ConstU32(PushData::XOffsetIndex))};
    const Id xoffset{ctx.OpLoad(type, xoffset_ptr)};
    const Id yoffset_ptr{ctx.OpAccessChain(ctx.TypePointer(spv::StorageClass::PushConstant, type),
                                           ctx.push_data_block,
                                           ctx.ConstU32(PushData::YOffsetIndex))};
    const Id yoffset{ctx.OpLoad(type, yoffset_ptr)};
    const Id xscale_ptr{ctx.OpAccessChain(ctx.TypePointer(spv::StorageClass::PushConstant, type),
                                          ctx.push_data_block,
                                          ctx.ConstU32(PushData::XScaleIndex))};
    const Id xscale{ctx.OpLoad(type, xscale_ptr)};
    const Id yscale_ptr{ctx.OpAccessChain(ctx.TypePointer(spv::StorageClass::PushConstant, type),
                                          ctx.push_data_block,
                                          ctx.ConstU32(PushData::YScaleIndex))};
    const Id yscale{ctx.OpLoad(type, yscale_ptr)};
    const Id vport_w =
        ctx.Constant(type, float(std::min<u32>(ctx.profile.max_viewport_width / 2, 8_KB)));
    const Id wnd_x = ctx.OpFAdd(type, ctx.OpFMul(type, x, xscale), xoffset);
    const Id ndc_x = ctx.OpFSub(type, ctx.OpFDiv(type, wnd_x, vport_w), ctx.Constant(type, 1.f));
    const Id vport_h =
        ctx.Constant(type, float(std::min<u32>(ctx.profile.max_viewport_height / 2, 8_KB)));
    const Id wnd_y = ctx.OpFAdd(type, ctx.OpFMul(type, y, yscale), yoffset);
    const Id ndc_y = ctx.OpFSub(type, ctx.OpFDiv(type, wnd_y, vport_h), ctx.Constant(type, 1.f));
    const Id vector{ctx.OpCompositeConstruct(ctx.F32[4], std::array<Id, 4>({ndc_x, ndc_y, z, w}))};
    ctx.OpStore(ctx.output_position, vector);
}

void EmitEpilogue(EmitContext& ctx) {
    if (ctx.stage == Stage::Vertex && ctx.runtime_info.vs_info.emulate_depth_negative_one_to_one) {
        ConvertDepthMode(ctx);
    }
    if (ctx.stage == Stage::Vertex && ctx.runtime_info.vs_info.clip_disable) {
        ConvertPositionToClipSpace(ctx);
    }
}

static bool IsCe3CoverageTarget(const EmitContext& ctx) {
    return DreamsCompat::CaptureCe3CoverageTrace() &&
           ctx.info.pgm_hash == DreamsCompat::Ce3ReadConstCaptureShader &&
           ctx.stage == Stage::Fragment && ctx.l_stage == LogicalStage::Fragment;
}

struct Ce3CoverageAccess {
    EmitContext& ctx;
    Id buffer_id;
    Id pointer_type;
    Id device_scope;
    Id acquire_release_semantics;
    Id selected_slot;
    Id accepted;

    Id Pointer(Id index) {
        return ctx.OpAccessChain(pointer_type, buffer_id, ctx.u32_zero_value, index);
    }

    Id PerSlotIndex(Id base) {
        return ctx.OpIAdd(ctx.U32[1], base, selected_slot);
    }

    void Increment(Id base, Id condition) {
        namespace Capture = DreamsCompat::Ce3CoverageTrace;
        const Id sink = ctx.ConstU32(Capture::SinkDword);
        const Id target = ctx.OpSelect(ctx.U32[1], accepted, PerSlotIndex(base), sink);
        const Id contributes = ctx.OpLogicalAnd(ctx.U1[1], accepted, condition);
        const Id increment =
            ctx.OpSelect(ctx.U32[1], contributes, ctx.u32_one_value, ctx.u32_zero_value);
        const Id previous = ctx.OpAtomicIAdd(ctx.U32[1], Pointer(target), device_scope,
                                             acquire_release_semantics, increment);
        const Id overflow = ctx.OpLogicalAnd(
            ctx.U1[1], contributes,
            ctx.OpIEqual(ctx.U1[1], previous, ctx.ConstU32(0xffffffffU)));
        ctx.OpAtomicIAdd(
            ctx.U32[1], Pointer(ctx.ConstU32(Capture::CounterOverflowDword)), device_scope,
            acquire_release_semantics,
            ctx.OpSelect(ctx.U32[1], overflow, ctx.u32_one_value, ctx.u32_zero_value));
    }

    void Increment(u32 base, Id condition) {
        Increment(ctx.ConstU32(base), condition);
    }
};

template <typename Callback>
static void EmitCe3CoverageEvent(EmitContext& ctx, Id miss_counter, Callback&& callback) {
    namespace Capture = DreamsCompat::Ce3CoverageTrace;
    const auto gds = std::ranges::find(ctx.buffers, BufferType::GdsBuffer,
                                       &EmitContext::BufferDefinition::buffer_type);
    ASSERT_MSG(gds != ctx.buffers.end(), "ce3 coverage trace has no GDS descriptor");
    const auto [buffer_id, pointer_type] = gds->Alias(EmitContext::PointerType::U32);
    const auto pointer = [&](Id index) {
        return ctx.OpAccessChain(pointer_type, buffer_id, ctx.u32_zero_value, index);
    };

    const Id arm = ctx.OpLoad(ctx.U32[1], pointer(ctx.ConstU32(Capture::ArmDword)));
    const Id armed = ctx.OpINotEqual(ctx.U1[1], arm, ctx.u32_zero_value);
    const Id armed_label = ctx.OpLabel();
    const Id merge_label = ctx.OpLabel();
    ctx.OpSelectionMerge(merge_label, spv::SelectionControlMask::MaskNone);
    ctx.OpBranchConditional(armed, armed_label, merge_label);

    ctx.AddLabel(armed_label);
    const Id device_scope = ctx.ConstU32(static_cast<u32>(spv::Scope::Device));
    const auto acquire_release = spv::MemorySemanticsMask::AcquireRelease |
                                 spv::MemorySemanticsMask::UniformMemory;
    const auto acquire =
        spv::MemorySemanticsMask::Acquire | spv::MemorySemanticsMask::UniformMemory;
    const Id acquire_release_semantics = ctx.ConstU32(static_cast<u32>(acquire_release));
    const Id acquire_semantics = ctx.ConstU32(static_cast<u32>(acquire));
    // Param1.y is observed as an exact, flat per-object label for this draw. Param1.x atlas records
    // can be reused by different objects, so using x here merges unrelated shapes and produces
    // misleading screen bounds.
    const Id candidate = ctx.OpBitcast(
        ctx.U32[1], EmitGetAttribute(ctx, IR::Attribute::Param1, 1, 0));
    const Id valid = ctx.OpINotEqual(ctx.U1[1], candidate, ctx.ConstU32(Capture::ClaimSentinel));

    Id hash = ctx.OpBitwiseXor(
        ctx.U32[1], candidate,
        ctx.OpShiftRightLogical(ctx.U32[1], candidate, ctx.ConstU32(16U)));
    hash = ctx.OpIMul(ctx.U32[1], hash, ctx.ConstU32(0x7feb352dU));
    hash = ctx.OpBitwiseXor(
        ctx.U32[1], hash,
        ctx.OpShiftRightLogical(ctx.U32[1], hash, ctx.ConstU32(15U)));
    hash = ctx.OpIMul(ctx.U32[1], hash, ctx.ConstU32(0x846ca68bU));
    hash = ctx.OpBitwiseXor(
        ctx.U32[1], hash,
        ctx.OpShiftRightLogical(ctx.U32[1], hash, ctx.ConstU32(16U)));
    const Id slot_mask = ctx.ConstU32(Capture::Slots - 1);
    const Id slot0 = ctx.OpBitwiseAnd(ctx.U32[1], hash, slot_mask);
    const Id step = ctx.OpBitwiseAnd(
        ctx.U32[1],
        ctx.OpBitwiseOr(
            ctx.U32[1],
            ctx.OpShiftRightLogical(ctx.U32[1], hash, ctx.ConstU32(Capture::SlotBits)),
            ctx.u32_one_value),
        slot_mask);
    const auto key_index = [&](Id slot) {
        return ctx.OpIAdd(ctx.U32[1], ctx.ConstU32(Capture::KeysBaseDword), slot);
    };
    Id accepted = ctx.false_value;
    Id selected_slot = slot0;
    for (u32 probe = 0; probe < Capture::Probes; ++probe) {
        const Id probe_offset = ctx.OpIMul(ctx.U32[1], step, ctx.ConstU32(probe));
        const Id slot = ctx.OpBitwiseAnd(
            ctx.U32[1], ctx.OpIAdd(ctx.U32[1], slot0, probe_offset), slot_mask);
        // Once this invocation has a slot, compare against the candidate instead of the sentinel.
        // That keeps every probe in straight-line SPIR-V without claiming any additional slots.
        const Id comparator = ctx.OpSelect(ctx.U32[1], accepted, candidate,
                                           ctx.ConstU32(Capture::ClaimSentinel));
        const Id previous = ctx.OpAtomicCompareExchange(
            ctx.U32[1], pointer(key_index(slot)), device_scope, acquire_release_semantics,
            acquire_semantics, candidate, comparator);
        const Id available = ctx.OpLogicalOr(
            ctx.U1[1],
            ctx.OpIEqual(ctx.U1[1], previous, ctx.ConstU32(Capture::ClaimSentinel)),
            ctx.OpIEqual(ctx.U1[1], previous, candidate));
        const Id accepted_here = ctx.OpLogicalAnd(
            ctx.U1[1], valid,
            ctx.OpLogicalAnd(ctx.U1[1], ctx.OpLogicalNot(ctx.U1[1], accepted), available));
        selected_slot = ctx.OpSelect(ctx.U32[1], accepted_here, slot, selected_slot);
        accepted = ctx.OpLogicalOr(ctx.U1[1], accepted, accepted_here);
    }
    const Id miss_target = ctx.OpSelect(
        ctx.U32[1], accepted, ctx.ConstU32(Capture::SinkDword), miss_counter);
    ctx.OpAtomicIAdd(
        ctx.U32[1], pointer(miss_target), device_scope, acquire_release_semantics,
        ctx.OpSelect(ctx.U32[1], accepted, ctx.u32_zero_value, ctx.u32_one_value));

    Ce3CoverageAccess access{ctx, buffer_id, pointer_type, device_scope,
                             acquire_release_semantics, selected_slot, accepted};
    callback(access);

    ctx.OpBranch(merge_label);
    ctx.AddLabel(merge_label);
}

static void EmitCe3CoverageCounter(EmitContext& ctx, Id counter_base, Id miss_counter,
                                   bool count_per_slot = true, u32 sample_site = 2,
                                   Id emitted = {}) {
    namespace Capture = DreamsCompat::Ce3CoverageTrace;
    EmitCe3CoverageEvent(ctx, miss_counter, [&](Ce3CoverageAccess& access) {
        if (count_per_slot) {
            access.Increment(counter_base, ctx.true_value);
        }

        if (sample_site >= 2) {
            return;
        }
        const Id sink = ctx.ConstU32(Capture::SinkDword);
        const Id sample_bins_base = ctx.ConstU32(
            sample_site == 0 ? Capture::InitialSampleBinsBaseDword
                             : Capture::RaymarchSampleBinsBaseDword);
        const Id sample_x = ctx.OpCompositeExtract(ctx.F32[1], emitted, 0);
        const Id below_half =
            ctx.OpFOrdLessThan(ctx.U1[1], sample_x, ctx.ConstF32(0.5f));
        const Id bin_increment =
            ctx.OpSelect(ctx.U32[1], below_half, ctx.u32_one_value,
                         ctx.ConstU32(Capture::PackedBinHighIncrement));
        const Id bin_index = access.PerSlotIndex(sample_bins_base);
        const Id bin_target = ctx.OpSelect(ctx.U32[1], access.accepted, bin_index, sink);
        const Id applied_increment =
            ctx.OpSelect(ctx.U32[1], access.accepted, bin_increment, ctx.u32_zero_value);
        const Id previous_bins =
            ctx.OpAtomicIAdd(ctx.U32[1], access.Pointer(bin_target), access.device_scope,
                             access.acquire_release_semantics, applied_increment);
        const Id previous_low = ctx.OpBitwiseAnd(
            ctx.U32[1], previous_bins, ctx.ConstU32(Capture::PackedBinMask));
        const Id previous_high = ctx.OpShiftRightLogical(
            ctx.U32[1], previous_bins, ctx.ConstU32(16U));
        const Id previous_selected =
            ctx.OpSelect(ctx.U32[1], below_half, previous_low, previous_high);
        const Id bin_overflow = ctx.OpLogicalAnd(
            ctx.U1[1], access.accepted,
            ctx.OpIEqual(ctx.U1[1], previous_selected,
                         ctx.ConstU32(Capture::PackedBinMask)));
        ctx.OpAtomicIAdd(
            ctx.U32[1], access.Pointer(ctx.ConstU32(Capture::SampleBinOverflowDword)),
            access.device_scope, access.acquire_release_semantics,
            ctx.OpSelect(ctx.U32[1], bin_overflow, ctx.u32_one_value,
                         ctx.u32_zero_value));

        if (sample_site == 0) {
            const Id param1_x = ctx.OpBitcast(
                ctx.U32[1], EmitGetAttribute(ctx, IR::Attribute::Param1, 0, 0));
            const Id frag_x = ctx.OpBitcast(
                ctx.U32[1], EmitGetAttribute(ctx, IR::Attribute::FragCoord, 0, 0));
            const Id frag_y = ctx.OpBitcast(
                ctx.U32[1], EmitGetAttribute(ctx, IR::Attribute::FragCoord, 1, 0));
            const auto metadata_target = [&](u32 base) {
                const Id index = access.PerSlotIndex(ctx.ConstU32(base));
                return access.Pointer(
                    ctx.OpSelect(ctx.U32[1], access.accepted, index, sink));
            };
            ctx.OpAtomicUMin(ctx.U32[1], metadata_target(Capture::Param1XMinBaseDword),
                             access.device_scope, access.acquire_release_semantics, param1_x);
            ctx.OpAtomicUMax(ctx.U32[1], metadata_target(Capture::Param1XMaxBaseDword),
                             access.device_scope, access.acquire_release_semantics, param1_x);
            ctx.OpAtomicUMin(ctx.U32[1], metadata_target(Capture::FragXMinBaseDword),
                             access.device_scope, access.acquire_release_semantics, frag_x);
            ctx.OpAtomicUMax(ctx.U32[1], metadata_target(Capture::FragXMaxBaseDword),
                             access.device_scope, access.acquire_release_semantics, frag_x);
            ctx.OpAtomicUMin(ctx.U32[1], metadata_target(Capture::FragYMinBaseDword),
                             access.device_scope, access.acquire_release_semantics, frag_y);
            ctx.OpAtomicUMax(ctx.U32[1], metadata_target(Capture::FragYMaxBaseDword),
                             access.device_scope, access.acquire_release_semantics, frag_y);
        }
    });
}

void EmitCe3CoverageDecision(EmitContext& ctx, u32 site, Id value, Id guest_negative) {
    if (!IsCe3CoverageTarget(ctx)) {
        return;
    }
    namespace Capture = DreamsCompat::Ce3CoverageTrace;
    ASSERT_MSG(site < Capture::DecisionSites, "ce3 decision site is outside the audited shader");
    EmitCe3CoverageEvent(
        ctx, ctx.ConstU32(Capture::MissDecisionBaseDword + site),
        [&](Ce3CoverageAccess& access) {
            const Id is_nan = ctx.OpIsNan(ctx.U1[1], value);
            const Id robust_negative = ctx.OpFOrdLessThan(
                ctx.U1[1], value, ctx.ConstF32(-Capture::DecisionEpsilon));
            const Id near_zero = ctx.OpFOrdLessThanEqual(
                ctx.U1[1], ctx.OpFAbs(ctx.F32[1], value),
                ctx.ConstF32(Capture::DecisionEpsilon));
            const Id category = ctx.OpSelect(
                ctx.U32[1], is_nan, ctx.ConstU32(Capture::DecisionNaN),
                ctx.OpSelect(
                    ctx.U32[1], robust_negative,
                    ctx.ConstU32(Capture::DecisionRobustNegative),
                    ctx.OpSelect(ctx.U32[1], near_zero,
                                 ctx.ConstU32(Capture::DecisionNearZero),
                                 ctx.ConstU32(Capture::DecisionRobustPositive))));
            const Id base = ctx.OpIAdd(
                ctx.U32[1], ctx.ConstU32(Capture::DecisionDword(site, 0)),
                ctx.OpIMul(ctx.U32[1], category, ctx.ConstU32(Capture::Slots)));
            access.Increment(base, ctx.true_value);
            access.Increment(Capture::DecisionGuestNegativeBaseDword + site * Capture::Slots,
                             guest_negative);
            access.Increment(Capture::DecisionGuestNonnegativeBaseDword +
                                 site * Capture::Slots,
                             ctx.OpLogicalNot(ctx.U1[1], guest_negative));
        });
}

void EmitCe3CoverageLoopExit(EmitContext& ctx, Id hit) {
    if (!IsCe3CoverageTarget(ctx)) {
        return;
    }
    namespace Capture = DreamsCompat::Ce3CoverageTrace;
    EmitCe3CoverageEvent(ctx, ctx.ConstU32(Capture::MissLoopDword),
                         [&](Ce3CoverageAccess& access) {
                             access.Increment(Capture::LoopHitBaseDword, hit);
                             access.Increment(Capture::LoopBoundNoHitBaseDword,
                                              ctx.OpLogicalNot(ctx.U1[1], hit));
                         });
}

void EmitCe3CoverageTrinary(EmitContext& ctx, u32 site, Id input0, Id input1, Id input2,
                            Id output) {
    if (!IsCe3CoverageTarget(ctx)) {
        return;
    }
    namespace Capture = DreamsCompat::Ce3CoverageTrace;
    ASSERT_MSG(site < Capture::TrinarySites, "ce3 trinary site is outside the audited shader");
    EmitCe3CoverageEvent(
        ctx, ctx.ConstU32(Capture::MissTrinaryBaseDword + site),
        [&](Ce3CoverageAccess& access) {
            access.Increment(Capture::TrinaryInvocationDword(site), ctx.true_value);
            const std::array values{input0, input1, input2, output};
            for (u32 value_index = 0; value_index < values.size(); ++value_index) {
                access.Increment(Capture::TrinaryNaNDword(site, value_index),
                                 ctx.OpIsNan(ctx.U1[1], values[value_index]));
            }
        });
}

void EmitCe3CoverageSample(EmitContext& ctx, u32 site, Id emitted) {
    if (!IsCe3CoverageTarget(ctx)) {
        return;
    }
    namespace Capture = DreamsCompat::Ce3CoverageTrace;
    ASSERT_MSG(site < 2, "ce3 coverage sample site is outside the audited shader");
    if (site == 0) {
        EmitCe3CoverageCounter(ctx, ctx.ConstU32(Capture::InitialSampleBaseDword),
                               ctx.ConstU32(Capture::MissInitialDword), true, site, emitted);
    } else {
        EmitCe3CoverageCounter(ctx, ctx.ConstU32(Capture::SinkDword),
                               ctx.ConstU32(Capture::MissRaymarchSampleDword), false, site,
                               emitted);
    }
}

void EmitCe3CoverageConditional(EmitContext& ctx, Id condition) {
    if (!IsCe3CoverageTarget(ctx)) {
        return;
    }
    namespace Capture = DreamsCompat::Ce3CoverageTrace;
    const Id counter = ctx.OpSelect(
        ctx.U32[1], condition, ctx.ConstU32(Capture::ConditionalTrueBaseDword),
        ctx.ConstU32(Capture::ConditionalFalseBaseDword));
    const Id miss =
        ctx.OpSelect(ctx.U32[1], condition,
                     ctx.ConstU32(Capture::MissConditionalTrueDword),
                     ctx.ConstU32(Capture::MissConditionalFalseDword));
    EmitCe3CoverageCounter(ctx, counter, miss);
}

void EmitCe3CoverageUnconditional(EmitContext& ctx) {
    if (!IsCe3CoverageTarget(ctx)) {
        return;
    }
    namespace Capture = DreamsCompat::Ce3CoverageTrace;
    EmitCe3CoverageCounter(ctx, ctx.ConstU32(Capture::UnconditionalBaseDword),
                           ctx.ConstU32(Capture::MissUnconditionalDword));
}

static void EmitCe3FleckUnconditionalDiscardCapture(EmitContext& ctx) {
    namespace Capture = DreamsCompat::Ce3FleckTrace;
    if (!DreamsCompat::CaptureCe3FleckTrace() ||
        ctx.info.pgm_hash != DreamsCompat::Ce3ReadConstCaptureShader ||
        ctx.stage != Stage::Fragment || ctx.l_stage != LogicalStage::Fragment) {
        return;
    }

    const auto gds = std::ranges::find(ctx.buffers, BufferType::GdsBuffer,
                                       &EmitContext::BufferDefinition::buffer_type);
    ASSERT_MSG(gds != ctx.buffers.end(), "ce3 fleck discard trace has no GDS descriptor");
    const auto [buffer_id, pointer_type] = gds->Alias(EmitContext::PointerType::U32);
    const auto pointer = [&](Id index) {
        return ctx.OpAccessChain(pointer_type, buffer_id, ctx.u32_zero_value, index);
    };

    const Id arm = ctx.OpLoad(ctx.U32[1], pointer(ctx.ConstU32(Capture::ArmDword)));
    const Id armed = ctx.OpINotEqual(ctx.U1[1], arm, ctx.u32_zero_value);
    const Id armed_label = ctx.OpLabel();
    const Id merge_label = ctx.OpLabel();
    ctx.OpSelectionMerge(merge_label, spv::SelectionControlMask::MaskNone);
    ctx.OpBranchConditional(armed, armed_label, merge_label);

    ctx.AddLabel(armed_label);
    const Id device_scope = ctx.ConstU32(static_cast<u32>(spv::Scope::Device));
    const auto acquire_release = spv::MemorySemanticsMask::AcquireRelease |
                                 spv::MemorySemanticsMask::UniformMemory;
    const auto acquire =
        spv::MemorySemanticsMask::Acquire | spv::MemorySemanticsMask::UniformMemory;
    const Id acquire_release_semantics = ctx.ConstU32(static_cast<u32>(acquire_release));
    const Id acquire_semantics = ctx.ConstU32(static_cast<u32>(acquire));
    const Id candidate = ctx.OpBitcast(
        ctx.U32[1], EmitGetAttribute(ctx, IR::Attribute::Param1, 0, 0));
    const Id claimed = ctx.OpAtomicLoad(
        ctx.U32[1], pointer(ctx.ConstU32(Capture::ClaimedParam1Dword)), device_scope,
        acquire_semantics);
    const Id id_matches = ctx.OpIEqual(ctx.U1[1], claimed, candidate);
    const Id match_label = ctx.OpLabel();
    const Id match_merge_label = ctx.OpLabel();
    ctx.OpSelectionMerge(match_merge_label, spv::SelectionControlMask::MaskNone);
    ctx.OpBranchConditional(id_matches, match_label, match_merge_label);

    ctx.AddLabel(match_label);
    ctx.OpAtomicIAdd(
        ctx.U32[1], pointer(ctx.ConstU32(Capture::UnconditionalDiscardInvocationsDword)),
        device_scope, acquire_release_semantics, ctx.u32_one_value);
    ctx.OpBranch(match_merge_label);
    ctx.AddLabel(match_merge_label);
    ctx.OpBranch(merge_label);
    ctx.AddLabel(merge_label);
}

void EmitDiscard(EmitContext& ctx) {
    EmitCe3CoverageUnconditional(ctx);
    EmitCe3FleckUnconditionalDiscardCapture(ctx);
    ctx.OpDemoteToHelperInvocationEXT();
}

static void EmitCe3FleckDiscardCapture(EmitContext& ctx, Id condition) {
    namespace Capture = DreamsCompat::Ce3FleckTrace;
    if (!DreamsCompat::CaptureCe3FleckTrace() ||
        ctx.info.pgm_hash != DreamsCompat::Ce3ReadConstCaptureShader ||
        ctx.stage != Stage::Fragment || ctx.l_stage != LogicalStage::Fragment) {
        return;
    }

    const auto gds = std::ranges::find(ctx.buffers, BufferType::GdsBuffer,
                                       &EmitContext::BufferDefinition::buffer_type);
    ASSERT_MSG(gds != ctx.buffers.end(), "ce3 fleck discard trace has no GDS descriptor");
    const auto [buffer_id, pointer_type] = gds->Alias(EmitContext::PointerType::U32);
    const auto pointer = [&](Id index) {
        return ctx.OpAccessChain(pointer_type, buffer_id, ctx.u32_zero_value, index);
    };

    const Id arm =
        ctx.OpLoad(ctx.U32[1], pointer(ctx.ConstU32(Capture::ArmDword)));
    const Id armed = ctx.OpINotEqual(ctx.U1[1], arm, ctx.u32_zero_value);
    const Id armed_label = ctx.OpLabel();
    const Id merge_label = ctx.OpLabel();
    ctx.OpSelectionMerge(merge_label, spv::SelectionControlMask::MaskNone);
    ctx.OpBranchConditional(armed, armed_label, merge_label);

    ctx.AddLabel(armed_label);
    const Id device_scope = ctx.ConstU32(static_cast<u32>(spv::Scope::Device));
    const auto acquire_release = spv::MemorySemanticsMask::AcquireRelease |
                                 spv::MemorySemanticsMask::UniformMemory;
    const auto acquire =
        spv::MemorySemanticsMask::Acquire | spv::MemorySemanticsMask::UniformMemory;
    const Id acquire_release_semantics =
        ctx.ConstU32(static_cast<u32>(acquire_release));
    const Id acquire_semantics = ctx.ConstU32(static_cast<u32>(acquire));
    const Id candidate = ctx.OpBitcast(
        ctx.U32[1], EmitGetAttribute(ctx, IR::Attribute::Param1, 0, 0));
    const Id claimed = ctx.OpAtomicLoad(
        ctx.U32[1], pointer(ctx.ConstU32(Capture::ClaimedParam1Dword)), device_scope,
        acquire_semantics);
    const Id id_matches = ctx.OpIEqual(ctx.U1[1], claimed, candidate);
    const Id match_label = ctx.OpLabel();
    const Id match_merge_label = ctx.OpLabel();
    ctx.OpSelectionMerge(match_merge_label, spv::SelectionControlMask::MaskNone);
    ctx.OpBranchConditional(id_matches, match_label, match_merge_label);

    ctx.AddLabel(match_label);
    ctx.OpAtomicIAdd(
        ctx.U32[1], pointer(ctx.ConstU32(Capture::ConditionalDiscardInvocationsDword)),
        device_scope, acquire_release_semantics, ctx.u32_one_value);
    const Id condition_counter = ctx.OpSelect(
        ctx.U32[1], condition, ctx.ConstU32(Capture::ConditionalDiscardTrueDword),
        ctx.ConstU32(Capture::ConditionalDiscardFalseDword));
    ctx.OpAtomicIAdd(ctx.U32[1], pointer(condition_counter), device_scope,
                     acquire_release_semantics, ctx.u32_one_value);
    constexpr u32 DiscardBit = 1U << Capture::StaticSampleSites;
    const Id previous = ctx.OpAtomicOr(
        ctx.U32[1], pointer(ctx.ConstU32(Capture::SeenDword)), device_scope,
        acquire_release_semantics, ctx.ConstU32(DiscardBit));
    const Id already_seen = ctx.OpBitwiseAnd(ctx.U32[1], previous,
                                             ctx.ConstU32(DiscardBit));
    const Id won = ctx.OpIEqual(ctx.U1[1], already_seen, ctx.u32_zero_value);
    const Id store_label = ctx.OpLabel();
    const Id store_merge_label = ctx.OpLabel();
    ctx.OpSelectionMerge(store_merge_label, spv::SelectionControlMask::MaskNone);
    ctx.OpBranchConditional(won, store_label, store_merge_label);

    ctx.AddLabel(store_label);
    ctx.OpStore(pointer(ctx.ConstU32(Capture::DiscardConditionDword)),
                ctx.OpSelect(ctx.U32[1], condition, ctx.u32_one_value, ctx.u32_zero_value));
    ctx.OpBranch(store_merge_label);
    ctx.AddLabel(store_merge_label);
    ctx.OpBranch(match_merge_label);
    ctx.AddLabel(match_merge_label);
    ctx.OpBranch(merge_label);
    ctx.AddLabel(merge_label);
}

void EmitDiscardCond(EmitContext& ctx, Id condition) {
    EmitCe3CoverageConditional(ctx, condition);
    EmitCe3FleckDiscardCapture(ctx, condition);
    const Id kill_label{ctx.OpLabel()};
    const Id merge_label{ctx.OpLabel()};
    ctx.OpSelectionMerge(merge_label, spv::SelectionControlMask::MaskNone);
    ctx.OpBranchConditional(condition, kill_label, merge_label);
    ctx.AddLabel(kill_label);
    ctx.OpDemoteToHelperInvocationEXT();
    ctx.OpBranch(merge_label);
    ctx.AddLabel(merge_label);
}

void EmitEmitVertex(EmitContext& ctx) {
    ctx.OpEmitVertex();
}

void EmitEmitPrimitive(EmitContext& ctx) {
    ctx.OpEndPrimitive();
}

void EmitEmitVertex(EmitContext& ctx, const IR::Value& stream) {
    UNREACHABLE_MSG("Geometry streams");
}

void EmitEndPrimitive(EmitContext& ctx, const IR::Value& stream) {
    UNREACHABLE_MSG("Geometry streams");
}

void EmitDebugPrint(EmitContext& ctx, IR::Inst* inst, Id fmt, Id arg0, Id arg1, Id arg2, Id arg3) {
    IR::DebugPrintFlags flags = inst->Flags<IR::DebugPrintFlags>();
    std::array<Id, IR::DEBUGPRINT_NUM_FORMAT_ARGS> fmt_args = {arg0, arg1, arg2, arg3};
    auto fmt_args_span = std::span<Id>(fmt_args.begin(), fmt_args.begin() + flags.num_args);
    ctx.OpDebugPrintf(fmt, fmt_args_span);
}

} // namespace Shader::Backend::SPIRV
