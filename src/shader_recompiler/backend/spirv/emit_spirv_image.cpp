// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <cstdlib>
#include <boost/container/static_vector.hpp>
#include "shader_recompiler/backend/spirv/emit_spirv_instructions.h"
#include "shader_recompiler/backend/spirv/spirv_emit_context.h"
#include "shader_recompiler/dreams_compat.h"
#include "shader_recompiler/ir/attribute.h"

namespace Shader::Backend::SPIRV {

struct ImageOperands {
    void Add(spv::ImageOperandsMask new_mask, Id value) {
        if (!Sirit::ValidId(value)) {
            return;
        }
        mask = static_cast<spv::ImageOperandsMask>(static_cast<u32>(mask) |
                                                   static_cast<u32>(new_mask));
        operands.push_back(value);
    }
    void Add(spv::ImageOperandsMask new_mask, Id value1, Id value2) {
        mask = static_cast<spv::ImageOperandsMask>(static_cast<u32>(mask) |
                                                   static_cast<u32>(new_mask));
        operands.push_back(value1);
        operands.push_back(value2);
    }

    void AddOffset(EmitContext& ctx, const IR::Value& offset,
                   bool can_use_runtime_offsets = false) {
        if (offset.IsEmpty()) {
            return;
        }
        if (offset.IsImmediate()) {
            const s32 operand = offset.U32();
            Add(spv::ImageOperandsMask::ConstOffset, ctx.ConstS32(operand));
            return;
        }
        IR::Inst* const inst{offset.InstRecursive()};
        if (inst->AreAllArgsImmediates()) {
            switch (inst->GetOpcode()) {
            case IR::Opcode::CompositeConstructU32x2:
                Add(spv::ImageOperandsMask::ConstOffset,
                    ctx.ConstS32(static_cast<s32>(inst->Arg(0).U32()),
                                 static_cast<s32>(inst->Arg(1).U32())));
                return;
            case IR::Opcode::CompositeConstructU32x3:
                Add(spv::ImageOperandsMask::ConstOffset,
                    ctx.ConstS32(static_cast<s32>(inst->Arg(0).U32()),
                                 static_cast<s32>(inst->Arg(1).U32()),
                                 static_cast<s32>(inst->Arg(2).U32())));
                return;
            default:
                break;
            }
        }
        if (can_use_runtime_offsets) {
            Add(spv::ImageOperandsMask::Offset, ctx.Def(offset));
        } else {
            LOG_WARNING(Render_Vulkan,
                        "Runtime offset provided to unsupported image sample instruction");
        }
    }

    void AddDerivatives(EmitContext& ctx, Id derivatives_dx, Id derivatives_dy) {
        if (!Sirit::ValidId(derivatives_dx) || !Sirit::ValidId(derivatives_dy)) {
            return;
        }
        Add(spv::ImageOperandsMask::Grad, derivatives_dx, derivatives_dy);
    }

    spv::ImageOperandsMask mask{};
    boost::container::static_vector<Id, 4> operands;
};

Id EmitImageSampleRaw(EmitContext& ctx, IR::Inst* inst, u32 handle, Id address1, Id address2,
                      Id address3, Id address4) {
    UNREACHABLE_MSG("Unreachable instruction");
}

Id EmitImageSampleImplicitLod(EmitContext& ctx, IR::Inst* inst, u32 handle, Id coords, Id bias,
                              const IR::Value& offset) {
    const auto& texture = ctx.images[handle & 0xFFFF];
    const Id image = ctx.OpLoad(texture.image_type, texture.id);
    const Id result_type = texture.data_types->Get(4);
    const Id sampler = ctx.OpLoad(ctx.sampler_type, ctx.samplers[handle >> 16]);
    const Id sampled_image = ctx.OpSampledImage(texture.sampled_type, image, sampler);
    ImageOperands operands;
    operands.Add(spv::ImageOperandsMask::Bias, bias);
    operands.AddOffset(ctx, offset, ctx.profile.supports_runtime_image_sample_offsets);
    const Id sample = ctx.OpImageSampleImplicitLod(result_type, sampled_image, coords,
                                                   operands.mask, operands.operands);
    return texture.is_integer ? ctx.OpBitcast(ctx.F32[4], sample) : sample;
}

static void EmitCe3FleckSampleCapture(EmitContext& ctx, u32 site, Id coords, Id emitted) {
    namespace Capture = DreamsCompat::Ce3FleckTrace;
    if (!DreamsCompat::CaptureCe3FleckTrace() ||
        ctx.info.pgm_hash != DreamsCompat::Ce3ReadConstCaptureShader ||
        ctx.stage != Stage::Fragment || ctx.l_stage != LogicalStage::Fragment ||
        site >= Capture::StaticSampleSites) {
        return;
    }

    const auto gds = std::ranges::find(ctx.buffers, BufferType::GdsBuffer,
                                       &EmitContext::BufferDefinition::buffer_type);
    ASSERT_MSG(gds != ctx.buffers.end(), "ce3 fleck trace has no GDS descriptor");
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
    const Id candidate = ctx.OpBitcast(
        ctx.U32[1], EmitGetAttribute(ctx, IR::Attribute::Param1, 0, 0));
    const Id device_scope = ctx.ConstU32(static_cast<u32>(spv::Scope::Device));
    const auto acquire_release = spv::MemorySemanticsMask::AcquireRelease |
                                 spv::MemorySemanticsMask::UniformMemory;
    const auto acquire =
        spv::MemorySemanticsMask::Acquire | spv::MemorySemanticsMask::UniformMemory;
    const Id acquire_release_semantics =
        ctx.ConstU32(static_cast<u32>(acquire_release));
    const Id acquire_semantics = ctx.ConstU32(static_cast<u32>(acquire));

    Id claimed{};
    if (site == 0) {
        const Id previous = ctx.OpAtomicCompareExchange(
            ctx.U32[1], pointer(ctx.ConstU32(Capture::ClaimedParam1Dword)), device_scope,
            acquire_release_semantics, acquire_semantics, candidate,
            ctx.ConstU32(Capture::ClaimSentinel));
        const Id won = ctx.OpIEqual(
            ctx.U1[1], previous, ctx.ConstU32(Capture::ClaimSentinel));
        claimed = ctx.OpSelect(ctx.U32[1], won, candidate, previous);
    } else {
        claimed = ctx.OpAtomicLoad(
            ctx.U32[1], pointer(ctx.ConstU32(Capture::ClaimedParam1Dword)), device_scope,
            acquire_semantics);
    }
    const Id id_matches = ctx.OpIEqual(ctx.U1[1], claimed, candidate);
    const Id match_label = ctx.OpLabel();
    const Id match_merge_label = ctx.OpLabel();
    ctx.OpSelectionMerge(match_merge_label, spv::SelectionControlMask::MaskNone);
    ctx.OpBranchConditional(id_matches, match_label, match_merge_label);

    ctx.AddLabel(match_label);
    if (site == 0) {
        ctx.OpAtomicIAdd(
            ctx.U32[1], pointer(ctx.ConstU32(Capture::InitialSampleInvocationsDword)),
            device_scope, acquire_release_semantics, ctx.u32_one_value);
    }
    const Id site_bit = ctx.ConstU32(1U << site);
    const Id previous_sites = ctx.OpAtomicOr(
        ctx.U32[1], pointer(ctx.ConstU32(Capture::SeenDword)), device_scope,
        acquire_release_semantics, site_bit);
    const Id already_seen =
        ctx.OpBitwiseAnd(ctx.U32[1], previous_sites, site_bit);
    const Id won_site = ctx.OpIEqual(ctx.U1[1], already_seen, ctx.u32_zero_value);
    const Id capture_label = ctx.OpLabel();
    const Id site_merge_label = ctx.OpLabel();
    ctx.OpSelectionMerge(site_merge_label, spv::SelectionControlMask::MaskNone);
    ctx.OpBranchConditional(won_site, capture_label, site_merge_label);

    ctx.AddLabel(capture_label);
    const u32 record = Capture::RecordDword(site, 0);
    const auto store = [&](u32 field, Id value) {
        ctx.OpStore(pointer(ctx.ConstU32(record + field)), value);
    };
    for (u32 component = 0; component < 4; ++component) {
        const Id frag = EmitGetAttribute(ctx, IR::Attribute::FragCoord, component, 0);
        const Id param = EmitGetAttribute(ctx, IR::Attribute::Param0, component, 0);
        store(Capture::FragCoord + component, ctx.OpBitcast(ctx.U32[1], frag));
        store(Capture::Param0 + component, ctx.OpBitcast(ctx.U32[1], param));
    }
    const Id coordinate_bits = ctx.OpBitcast(ctx.U32[3], coords);
    for (u32 component = 0; component < 3; ++component) {
        store(Capture::Coordinates + component,
              ctx.OpCompositeExtract(ctx.U32[1], coordinate_bits, component));
    }
    const Id sample_bits = ctx.OpBitcast(ctx.U32[4], emitted);
    for (u32 component = 0; component < 4; ++component) {
        store(Capture::Sample + component,
              ctx.OpCompositeExtract(ctx.U32[1], sample_bits, component));
    }
    ctx.OpBranch(site_merge_label);

    ctx.AddLabel(site_merge_label);
    ctx.OpBranch(match_merge_label);
    ctx.AddLabel(match_merge_label);
    ctx.OpBranch(merge_label);
    ctx.AddLabel(merge_label);
}

Id EmitImageSampleExplicitLod(EmitContext& ctx, IR::Inst* inst, u32 handle, Id coords, Id lod,
                              const IR::Value& offset) {
    const auto& texture = ctx.images[handle & 0xFFFF];
    const Id image = ctx.OpLoad(texture.image_type, texture.id);
    const Id result_type = texture.data_types->Get(4);
    const Id sampler = ctx.OpLoad(ctx.sampler_type, ctx.samplers[handle >> 16]);
    const Id sampled_image = ctx.OpSampledImage(texture.sampled_type, image, sampler);
    ImageOperands operands;
    operands.Add(spv::ImageOperandsMask::Lod, lod);
    operands.AddOffset(ctx, offset, ctx.profile.supports_runtime_image_sample_offsets);
    const Id sample = ctx.OpImageSampleExplicitLod(result_type, sampled_image, coords,
                                                   operands.mask, operands.operands);
    const Id emitted = texture.is_integer ? ctx.OpBitcast(ctx.F32[4], sample) : sample;
    if ((DreamsCompat::CaptureCe3FleckTrace() || DreamsCompat::CaptureCe3CoverageTrace()) &&
        ctx.info.pgm_hash == DreamsCompat::Ce3ReadConstCaptureShader &&
        ctx.stage == Stage::Fragment && ctx.l_stage == LogicalStage::Fragment &&
        (handle & 0xffff) == 0) {
        const u32 site = ctx.dreams_ce3_fleck_sample_ordinal++;
        if (DreamsCompat::CaptureCe3FleckTrace()) {
            EmitCe3FleckSampleCapture(ctx, site, coords, emitted);
        }
        if (DreamsCompat::CaptureCe3CoverageTrace() && site < 2) {
            EmitCe3CoverageSample(ctx, site, emitted);
        }
    }
    return emitted;
}

Id EmitImageSampleDrefImplicitLod(EmitContext& ctx, IR::Inst* inst, u32 handle, Id coords, Id dref,
                                  Id bias, const IR::Value& offset) {
    const auto& texture = ctx.images[handle & 0xFFFF];
    const Id image = ctx.OpLoad(texture.image_type, texture.id);
    const Id result_type = texture.data_types->Get(1);
    const Id sampler = ctx.OpLoad(ctx.sampler_type, ctx.samplers[handle >> 16]);
    const Id sampled_image = ctx.OpSampledImage(texture.sampled_type, image, sampler);
    ImageOperands operands;
    operands.Add(spv::ImageOperandsMask::Bias, bias);
    operands.AddOffset(ctx, offset, ctx.profile.supports_runtime_image_sample_offsets);
    const Id sample = ctx.OpImageSampleDrefImplicitLod(result_type, sampled_image, coords, dref,
                                                       operands.mask, operands.operands);
    const Id sample_typed = texture.is_integer ? ctx.OpBitcast(ctx.F32[1], sample) : sample;
    return ctx.OpCompositeConstruct(ctx.F32[4], sample_typed, ctx.f32_zero_value,
                                    ctx.f32_zero_value, ctx.f32_zero_value);
}

Id EmitImageSampleDrefExplicitLod(EmitContext& ctx, IR::Inst* inst, u32 handle, Id coords, Id dref,
                                  Id lod, const IR::Value& offset) {
    const auto& texture = ctx.images[handle & 0xFFFF];
    const Id image = ctx.OpLoad(texture.image_type, texture.id);
    const Id result_type = texture.data_types->Get(1);
    const Id sampler = ctx.OpLoad(ctx.sampler_type, ctx.samplers[handle >> 16]);
    const Id sampled_image = ctx.OpSampledImage(texture.sampled_type, image, sampler);
    ImageOperands operands;
    operands.Add(spv::ImageOperandsMask::Lod, lod);
    operands.AddOffset(ctx, offset, ctx.profile.supports_runtime_image_sample_offsets);
    const Id sample = ctx.OpImageSampleDrefExplicitLod(result_type, sampled_image, coords, dref,
                                                       operands.mask, operands.operands);
    const Id sample_typed = texture.is_integer ? ctx.OpBitcast(ctx.F32[1], sample) : sample;
    return ctx.OpCompositeConstruct(ctx.F32[4], sample_typed, ctx.f32_zero_value,
                                    ctx.f32_zero_value, ctx.f32_zero_value);
}

void EmitImageGather3DCapture(EmitContext& ctx, u32 site, u32 image_binding, u32 component,
                              u32 z_filter, Id coords, Id dimensions,
                              const std::array<Id, 4>& source_coords,
                              const std::array<Id, 4>& source_texels,
                              const std::array<Id, 4>& selected_components, Id emitted) {
    if (!DreamsCompat::CaptureImageGather3DForShader(ctx.info.pgm_hash) ||
        ctx.stage != Stage::Compute || ctx.l_stage != LogicalStage::Compute ||
        site >= DreamsCompat::ImageGather3DCapture::MaxSites) {
        return;
    }

    ASSERT_MSG(Sirit::ValidId(ctx.workgroup_id) && Sirit::ValidId(ctx.local_invocation_id),
               "3D ImageGather capture has no compute invocation IDs");
    const auto gds = std::ranges::find(ctx.buffers, BufferType::GdsBuffer,
                                       &EmitContext::BufferDefinition::buffer_type);
    ASSERT_MSG(gds != ctx.buffers.end(), "3D ImageGather capture has no GDS descriptor");
    const auto [buffer_id, pointer_type] = gds->Alias(EmitContext::PointerType::U32);
    const auto pointer = [&](Id index) {
        return ctx.OpAccessChain(pointer_type, buffer_id, ctx.u32_zero_value, index);
    };

    namespace Window = DreamsCompat::ImageGather3DSampleWindow;
    Id in_sample_window = ctx.true_value;
    if (DreamsCompat::CaptureSculptAtlasLineage()) {
        const Id window_workgroup = ctx.OpLoad(ctx.U32[3], ctx.workgroup_id);
        const Id window_local = ctx.OpLoad(ctx.U32[3], ctx.local_invocation_id);
        for (u32 axis = 0; axis < 3; ++axis) {
            const Id workgroup_in_range =
                ctx.OpULessThan(ctx.U1[1],
                                ctx.OpCompositeExtract(ctx.U32[1], window_workgroup, axis),
                                ctx.ConstU32(Window::WorkgroupsPerAxis));
            const Id local_in_range =
                ctx.OpULessThan(ctx.U1[1],
                                ctx.OpCompositeExtract(ctx.U32[1], window_local, axis),
                                ctx.ConstU32(Window::LocalInvocationsPerAxis));
            in_sample_window =
                ctx.OpLogicalAnd(ctx.U1[1], in_sample_window,
                                 ctx.OpLogicalAnd(ctx.U1[1], workgroup_in_range, local_in_range));
        }
        if (ctx.info.pgm_hash == DreamsCompat::SculptSurfaceCompactShader) {
            ASSERT_MSG(Sirit::ValidId(ctx.dreams_ordered_phase),
                       "f030 gather capture has no ordered-phase specialization");
            const Id is_replay = ctx.OpIEqual(ctx.U1[1], ctx.dreams_ordered_phase,
                                              ctx.ConstU32(DreamsCompat::OrderedPhaseReplay));
            in_sample_window = ctx.OpLogicalAnd(ctx.U1[1], in_sample_window, is_replay);
        }
    }

    const Id arm_check_label = ctx.OpLabel();
    const Id claim_label = ctx.OpLabel();
    const Id capture_label = ctx.OpLabel();
    const Id claim_done_label = ctx.OpLabel();
    const Id arm_done_label = ctx.OpLabel();
    const Id merge_label = ctx.OpLabel();
    ctx.OpSelectionMerge(merge_label, spv::SelectionControlMask::MaskNone);
    ctx.OpBranchConditional(in_sample_window, arm_check_label, merge_label);

    ctx.AddLabel(arm_check_label);
    const Id arm = ctx.OpLoad(
        ctx.U32[1], pointer(ctx.ConstU32(DreamsCompat::ImageGather3DCapture::ArmDword)));
    const Id target_hash_lo = ctx.OpLoad(
        ctx.U32[1],
        pointer(ctx.ConstU32(DreamsCompat::ImageGather3DCapture::TargetHashLoDword)));
    const Id target_hash_hi = ctx.OpLoad(
        ctx.U32[1],
        pointer(ctx.ConstU32(DreamsCompat::ImageGather3DCapture::TargetHashHiDword)));
    const Id is_armed = ctx.OpINotEqual(ctx.U1[1], arm, ctx.u32_zero_value);
    const Id hash_lo_matches = ctx.OpIEqual(
        ctx.U1[1], target_hash_lo, ctx.ConstU32(static_cast<u32>(ctx.info.pgm_hash)));
    const Id hash_hi_matches = ctx.OpIEqual(
        ctx.U1[1], target_hash_hi, ctx.ConstU32(static_cast<u32>(ctx.info.pgm_hash >> 32)));
    const Id should_capture = ctx.OpLogicalAnd(
        ctx.U1[1], is_armed,
        ctx.OpLogicalAnd(ctx.U1[1], hash_lo_matches, hash_hi_matches));
    ctx.OpSelectionMerge(arm_done_label, spv::SelectionControlMask::MaskNone);
    ctx.OpBranchConditional(should_capture, claim_label, arm_done_label);

    const Id device_scope = ctx.ConstU32(static_cast<u32>(spv::Scope::Device));
    const auto memory_semantics = spv::MemorySemanticsMask::AcquireRelease |
                                  spv::MemorySemanticsMask::UniformMemory;
    const Id semantics = ctx.ConstU32(static_cast<u32>(memory_semantics));
    const Id site_bit = ctx.ConstU32(1U << site);
    ctx.AddLabel(claim_label);
    const Id previous_claims = ctx.OpAtomicOr(
        ctx.U32[1],
        pointer(ctx.ConstU32(DreamsCompat::ImageGather3DCapture::ClaimDword)), device_scope,
        semantics, site_bit);
    const Id already_claimed =
        ctx.OpBitwiseAnd(ctx.U32[1], previous_claims, site_bit);
    const Id won_claim =
        ctx.OpIEqual(ctx.U1[1], already_claimed, ctx.u32_zero_value);
    ctx.OpSelectionMerge(claim_done_label, spv::SelectionControlMask::MaskNone);
    ctx.OpBranchConditional(won_claim, capture_label, claim_done_label);

    ctx.AddLabel(capture_label);
    const Id workgroup = ctx.OpLoad(ctx.U32[3], ctx.workgroup_id);
    const Id local = ctx.OpLoad(ctx.U32[3], ctx.local_invocation_id);
    const u32 record = DreamsCompat::ImageGather3DCapture::RecordDword(site, 0);
    const auto store = [&](u32 field, Id value) {
        ctx.OpStore(pointer(ctx.ConstU32(record + field)), value);
    };
    store(DreamsCompat::ImageGather3DCapture::ShaderHashLo,
          ctx.ConstU32(static_cast<u32>(ctx.info.pgm_hash)));
    store(DreamsCompat::ImageGather3DCapture::ShaderHashHi,
          ctx.ConstU32(static_cast<u32>(ctx.info.pgm_hash >> 32)));
    store(DreamsCompat::ImageGather3DCapture::Stage,
          ctx.ConstU32(static_cast<u32>(ctx.stage)));
    store(DreamsCompat::ImageGather3DCapture::Site, ctx.ConstU32(site));
    store(DreamsCompat::ImageGather3DCapture::Component, ctx.ConstU32(component));
    store(DreamsCompat::ImageGather3DCapture::ZFilter, ctx.ConstU32(z_filter));
    store(DreamsCompat::ImageGather3DCapture::ImageBinding, ctx.ConstU32(image_binding));
    for (u32 axis = 0; axis < 3; ++axis) {
        store(DreamsCompat::ImageGather3DCapture::Workgroup + axis,
              ctx.OpCompositeExtract(ctx.U32[1], workgroup, axis));
        store(DreamsCompat::ImageGather3DCapture::LocalInvocation + axis,
              ctx.OpCompositeExtract(ctx.U32[1], local, axis));
        const Id normalized = ctx.OpCompositeExtract(ctx.F32[1], coords, axis);
        store(DreamsCompat::ImageGather3DCapture::NormalizedCoordinates + axis,
              ctx.OpBitcast(ctx.U32[1], normalized));
        store(DreamsCompat::ImageGather3DCapture::Dimensions + axis,
              ctx.OpCompositeExtract(ctx.U32[1], dimensions, axis));
    }
    for (u32 source = 0; source < source_coords.size(); ++source) {
        for (u32 axis = 0; axis < 3; ++axis) {
            const Id source_coord =
                ctx.OpCompositeExtract(ctx.S32[1], source_coords[source], axis);
            store(DreamsCompat::ImageGather3DCapture::SourceCoordinates + source * 3 + axis,
                  ctx.OpBitcast(ctx.U32[1], source_coord));
        }
        const Id texel_bits = ctx.OpBitcast(ctx.U32[4], source_texels[source]);
        for (u32 channel = 0; channel < 4; ++channel) {
            store(DreamsCompat::ImageGather3DCapture::SourceTexels + source * 4 + channel,
                  ctx.OpCompositeExtract(ctx.U32[1], texel_bits, channel));
        }
        store(DreamsCompat::ImageGather3DCapture::SelectedComponents + source,
              ctx.OpBitcast(ctx.U32[1], selected_components[source]));
        const Id emitted_component = ctx.OpCompositeExtract(ctx.F32[1], emitted, source);
        store(DreamsCompat::ImageGather3DCapture::EmittedComponents + source,
              ctx.OpBitcast(ctx.U32[1], emitted_component));
    }
    ctx.OpAtomicOr(
        ctx.U32[1],
        pointer(ctx.ConstU32(DreamsCompat::ImageGather3DCapture::SeenDword)), device_scope,
        semantics, site_bit);
    ctx.OpBranch(claim_done_label);
    ctx.AddLabel(claim_done_label);
    ctx.OpBranch(arm_done_label);
    ctx.AddLabel(arm_done_label);
    ctx.OpBranch(merge_label);
    ctx.AddLabel(merge_label);
}

Id EmitImageGather(EmitContext& ctx, IR::Inst* inst, u32 handle, Id coords,
                   const IR::Value& offset) {
    const auto& texture = ctx.images[handle & 0xFFFF];
    const Id image = ctx.OpLoad(texture.image_type, texture.id);
    const Id result_type = texture.data_types->Get(4);
    const Id sampler = ctx.OpLoad(ctx.sampler_type, ctx.samplers[handle >> 16]);
    const Id sampled_image = ctx.OpSampledImage(texture.sampled_type, image, sampler);
    const u32 comp = inst->Flags<IR::TextureInstInfo>().gather_comp.Value();
    if (texture.view_type == AmdGpu::ImageType::Color3D ||
        texture.view_type == AmdGpu::ImageType::Color1D ||
        texture.view_type == AmdGpu::ImageType::Color1DArray) {
        const char* offset_kind = offset.IsEmpty()       ? "empty"
                                  : offset.IsImmediate() ? "immediate"
                                                         : "dynamic";
        const u32 immediate_offset = offset.IsImmediate() ? offset.U32() : 0;
        const auto image_sharp = ctx.info.images[handle & 0xFFFF].GetSharp(ctx.info);
        const auto sampler_sharp = ctx.info.samplers[handle >> 16].GetSharp(ctx.info);
        LOG_WARNING(Render,
                    "Shader {:#x} {}: ImageGather fallback view={} image={} sampler={} comp={} "
                    "integer={} offset={} value={:#x} size={}x{}x{} sampler_raw={:#x}/{:#x}",
                    ctx.info.pgm_hash, ctx.stage, static_cast<u64>(texture.view_type),
                    handle & 0xFFFF, handle >> 16, comp, texture.is_integer, offset_kind,
                    immediate_offset, image_sharp.width + 1, image_sharp.height + 1,
                    image_sharp.depth + 1, sampler_sharp.raw0, sampler_sharp.raw1);
        if (texture.view_type == AmdGpu::ImageType::Color3D && offset.IsEmpty() &&
            sampler_sharp.clamp_x == AmdGpu::ClampMode::Wrap &&
            sampler_sharp.clamp_y == AmdGpu::ClampMode::Wrap &&
            sampler_sharp.clamp_z == AmdGpu::ClampMode::Wrap) {
            // SPIR-V cannot gather from a 3D image. Recreate the 2x2 XY footprint with four
            // integer fetches while selecting Z as a point-sampled coordinate.
            const Id dimensions =
                ctx.OpImageQuerySizeLod(ctx.U32[3], image, ctx.u32_zero_value);
            const Id signed_dimensions = ctx.OpBitcast(ctx.S32[3], dimensions);

            const auto scaled_coord = [&](u32 index) {
                const Id coord = ctx.OpCompositeExtract(ctx.F32[1], coords, index);
                const Id dimension = ctx.OpCompositeExtract(ctx.U32[1], dimensions, index);
                const Id dimension_f = ctx.OpConvertUToF(ctx.F32[1], dimension);
                return ctx.OpFMul(ctx.F32[1], coord, dimension_f);
            };
            const auto wrap_coord = [&](Id coord, u32 index) {
                const Id dimension =
                    ctx.OpCompositeExtract(ctx.S32[1], signed_dimensions, index);
                return ctx.OpSMod(ctx.S32[1], coord, dimension);
            };

            const Id x_base_f = ctx.OpFloor(
                ctx.F32[1],
                ctx.OpFSub(ctx.F32[1], scaled_coord(0), ctx.ConstF32(0.5f)));
            const Id y_base_f = ctx.OpFloor(
                ctx.F32[1],
                ctx.OpFSub(ctx.F32[1], scaled_coord(1), ctx.ConstF32(0.5f)));
            const Id z_base_f = ctx.OpFloor(ctx.F32[1], scaled_coord(2));
            const Id x_base = ctx.OpConvertFToS(ctx.S32[1], x_base_f);
            const Id y_base = ctx.OpConvertFToS(ctx.S32[1], y_base_f);
            const Id z_base = ctx.OpConvertFToS(ctx.S32[1], z_base_f);
            const Id x0 = wrap_coord(x_base, 0);
            const Id y0 = wrap_coord(y_base, 1);
            const Id z0 = wrap_coord(z_base, 2);
            const Id x1 = wrap_coord(
                ctx.OpIAdd(ctx.S32[1], x_base, ctx.ConstS32(1)), 0);
            const Id y1 = wrap_coord(
                ctx.OpIAdd(ctx.S32[1], y_base, ctx.ConstS32(1)), 1);

            std::array<Id, 4> fetch_coords{};
            std::array<Id, 4> fetched_texels{};
            std::array<Id, 4> selected_components{};
            const auto fetch_component = [&](u32 source, Id x, Id y) {
                fetch_coords[source] = ctx.OpCompositeConstruct(ctx.S32[3], x, y, z0);
                fetched_texels[source] =
                    ctx.OpImageFetch(result_type, image, fetch_coords[source],
                                     spv::ImageOperandsMask::Lod, ctx.u32_zero_value);
                selected_components[source] = ctx.OpCompositeExtract(
                    texture.data_types->Get(1), fetched_texels[source], comp);
                return selected_components[source];
            };
            const Id i0j1 = fetch_component(0, x0, y1);
            const Id i1j1 = fetch_component(1, x1, y1);
            const Id i1j0 = fetch_component(2, x1, y0);
            const Id i0j0 = fetch_component(3, x0, y0);
            const Id texels =
                ctx.OpCompositeConstruct(result_type, i0j1, i1j1, i1j0, i0j0);
            const Id emitted = texture.is_integer ? ctx.OpBitcast(ctx.F32[4], texels) : texels;
            const u32 capture_site = ctx.dreams_image_gather_3d_capture_ordinal++;
            EmitImageGather3DCapture(ctx, capture_site, handle & 0xffff, comp,
                                     static_cast<u32>(sampler_sharp.z_filter.Value()), coords,
                                     dimensions, fetch_coords, fetched_texels,
                                     selected_components, emitted);
            return emitted;
        }
        const Id sample = ctx.OpImageSampleExplicitLod(
            result_type, sampled_image, coords, spv::ImageOperandsMask::Lod, ctx.ConstF32(0.f));
        const Id sample_typed = texture.is_integer ? ctx.OpBitcast(ctx.F32[4], sample) : sample;
        const Id component = ctx.OpCompositeExtract(ctx.F32[1], sample_typed, comp);
        return ctx.OpCompositeConstruct(ctx.F32[4], component, component, component, component);
    }
    ImageOperands operands;
    operands.AddOffset(ctx, offset, true);
    const Id texels = ctx.OpImageGather(result_type, sampled_image, coords, ctx.ConstU32(comp),
                                        operands.mask, operands.operands);
    return texture.is_integer ? ctx.OpBitcast(ctx.F32[4], texels) : texels;
}

Id EmitImageGatherDref(EmitContext& ctx, IR::Inst* inst, u32 handle, Id coords,
                       const IR::Value& offset, Id dref) {
    const auto& texture = ctx.images[handle & 0xFFFF];
    const Id image = ctx.OpLoad(texture.image_type, texture.id);
    const Id result_type = texture.data_types->Get(4);
    const Id sampler = ctx.OpLoad(ctx.sampler_type, ctx.samplers[handle >> 16]);
    const Id sampled_image = ctx.OpSampledImage(texture.sampled_type, image, sampler);
    if (texture.view_type == AmdGpu::ImageType::Color3D ||
        texture.view_type == AmdGpu::ImageType::Color1D ||
        texture.view_type == AmdGpu::ImageType::Color1DArray) {
        LOG_WARNING(Render, "Fallback for ImageGatherDref with unsupported image dimension");
        const Id sample = ctx.OpImageSampleDrefExplicitLod(
            texture.data_types->Get(1), sampled_image, coords, dref, spv::ImageOperandsMask::Lod,
            ctx.ConstF32(0.f));
        const Id sample_typed = texture.is_integer ? ctx.OpBitcast(ctx.F32[1], sample) : sample;
        return ctx.OpCompositeConstruct(ctx.F32[4], sample_typed, sample_typed, sample_typed,
                                        sample_typed);
    }
    ImageOperands operands;
    operands.AddOffset(ctx, offset, true);
    const Id texels = ctx.OpImageDrefGather(result_type, sampled_image, coords, dref, operands.mask,
                                            operands.operands);
    return texture.is_integer ? ctx.OpBitcast(ctx.F32[4], texels) : texels;
}

Id EmitImageQueryDimensions(EmitContext& ctx, IR::Inst* inst, u32 handle, Id lod, bool has_mips) {
    const auto& texture = ctx.images[handle & 0xFFFF];
    const Id image = ctx.OpLoad(texture.image_type, texture.id);
    const auto sharp = ctx.info.images[handle & 0xFFFF].GetSharp(ctx.info);
    const Id zero = ctx.u32_zero_value;
    const auto mips{[&] { return has_mips ? ctx.OpImageQueryLevels(ctx.U32[1], image) : zero; }};
    const bool uses_lod{texture.view_type != AmdGpu::ImageType::Color2DMsaa && !texture.is_storage};
    const auto query{[&](Id type) {
        return uses_lod ? ctx.OpImageQuerySizeLod(type, image, lod)
                        : ctx.OpImageQuerySize(type, image);
    }};
    switch (texture.view_type) {
    case AmdGpu::ImageType::Color1D:
        return ctx.OpCompositeConstruct(ctx.U32[4], query(ctx.U32[1]), zero, zero, mips());
    case AmdGpu::ImageType::Color1DArray:
    case AmdGpu::ImageType::Color2D:
    case AmdGpu::ImageType::Color2DMsaa:
        return ctx.OpCompositeConstruct(ctx.U32[4], query(ctx.U32[2]), zero, mips());
    case AmdGpu::ImageType::Color2DArray:
    case AmdGpu::ImageType::Color3D:
        return ctx.OpCompositeConstruct(ctx.U32[4], query(ctx.U32[3]), mips());
    default:
        UNREACHABLE_MSG("SPIR-V Instruction");
    }
}

Id EmitImageQueryLod(EmitContext& ctx, IR::Inst* inst, u32 handle, Id coords) {
    const auto& texture = ctx.images[handle & 0xFFFF];
    const Id image = ctx.OpLoad(texture.image_type, texture.id);
    const Id sampler = ctx.OpLoad(ctx.sampler_type, ctx.samplers[handle >> 16]);
    const Id sampled_image = ctx.OpSampledImage(texture.sampled_type, image, sampler);
    const Id zero{ctx.f32_zero_value};
    return ctx.OpImageQueryLod(ctx.F32[2], sampled_image, coords);
}

Id EmitImageGradient(EmitContext& ctx, IR::Inst* inst, u32 handle, Id coords, Id derivatives_dx,
                     Id derivatives_dy, const IR::Value& offset, const IR::Value& lod_clamp) {
    const auto& texture = ctx.images[handle & 0xFFFF];
    const Id image = ctx.OpLoad(texture.image_type, texture.id);
    const Id result_type = texture.data_types->Get(4);
    const Id sampler = ctx.OpLoad(ctx.sampler_type, ctx.samplers[handle >> 16]);
    const Id sampled_image = ctx.OpSampledImage(texture.sampled_type, image, sampler);
    ImageOperands operands;
    operands.AddDerivatives(ctx, derivatives_dx, derivatives_dy);
    operands.AddOffset(ctx, offset, ctx.profile.supports_runtime_image_sample_offsets);
    const Id sample = ctx.OpImageSampleExplicitLod(result_type, sampled_image, coords,
                                                   operands.mask, operands.operands);
    return texture.is_integer ? ctx.OpBitcast(ctx.F32[4], sample) : sample;
}

Id EmitImageRead(EmitContext& ctx, IR::Inst* inst, u32 handle, Id coords, Id lod, Id ms) {
    const auto& texture = ctx.images[handle & 0xFFFF];
    const Id color_type = texture.data_types->Get(4);
    ImageOperands operands;
    operands.Add(spv::ImageOperandsMask::Sample, ms);
    Id texel;
    if (!texture.is_storage) {
        const Id image = ctx.OpLoad(texture.image_type, texture.id);
        if (texture.view_type != AmdGpu::ImageType::Color2DMsaa) {
            if (Sirit::ValidId(ms)) {
                LOG_ERROR(Render_Recompiler, "image is not MS but ms operand is provided");
            }
            operands.Add(spv::ImageOperandsMask::Lod, lod);
        }
        texel = ctx.OpImageFetch(color_type, image, coords, operands.mask, operands.operands);
    } else {
        Id image_ptr = texture.id;
        if (ctx.profile.supports_image_load_store_lod) {
            operands.Add(spv::ImageOperandsMask::Lod, lod);
        } else if (Sirit::ValidId(lod)) {
#if 1
            // It's  confusing what interactions will cause this code path so leave it as
            // unreachable until a case is found.
            // Normally IMAGE_LOAD_MIP should translate -> OpImageFetch
            UNREACHABLE_MSG("Unsupported ImageRead with Lod");
#else
            LOG_WARNING(Render, "Fallback for ImageRead with LOD");
            ASSERT(texture.mip_fallback_mode == MipStorageFallbackMode::DynamicIndex);
            const Id single_image_ptr_type =
                ctx.TypePointer(spv::StorageClass::UniformConstant, texture.image_type);
            image_ptr = ctx.OpAccessChain(single_image_ptr_type, image_ptr, std::array{lod});
#endif
        }
        const Id image = ctx.OpLoad(texture.image_type, image_ptr);
        texel = ctx.OpImageRead(color_type, image, coords, operands.mask, operands.operands);
    }
    return texture.is_integer ? ctx.OpBitcast(ctx.F32[4], texel) : texel;
}

void EmitImageWrite(EmitContext& ctx, IR::Inst* inst, u32 handle, Id coords, Id lod, Id ms,
                    Id color) {
    if (ctx.info.pgm_hash == 0x7ba4de5d &&
        std::getenv("SHADPS4_DREAMS_GATHER_DISABLE_IMAGE_WRITES") != nullptr) {
        return;
    }
    const auto& texture = ctx.images[handle & 0xFFFF];
    Id image_ptr = texture.id;
    const Id color_type = texture.data_types->Get(4);
    ImageOperands operands;
    operands.Add(spv::ImageOperandsMask::Sample, ms);
    if (ctx.profile.supports_image_load_store_lod) {
        operands.Add(spv::ImageOperandsMask::Lod, lod);
    } else if (Sirit::ValidId(lod)) {
        LOG_WARNING(Render, "Fallback for ImageWrite with LOD");
        ASSERT(texture.mip_fallback_mode == MipStorageFallbackMode::DynamicIndex);
        const Id single_image_ptr_type =
            ctx.TypePointer(spv::StorageClass::UniformConstant, texture.image_type);
        image_ptr = ctx.OpAccessChain(single_image_ptr_type, image_ptr, std::array{lod});
    }
    const Id image = ctx.OpLoad(texture.image_type, image_ptr);
    const Id texel = texture.is_integer ? ctx.OpBitcast(color_type, color) : color;
    ctx.OpImageWrite(image, coords, texel, operands.mask, operands.operands);
}

Id EmitCubeFaceIndex(EmitContext& ctx, IR::Inst* inst, Id cube_coords) {
    if (ctx.profile.supports_native_cube_calc) {
        return ctx.OpCubeFaceIndexAMD(ctx.F32[1], cube_coords);
    } else {
        UNREACHABLE_MSG("SPIR-V Instruction");
    }
}

} // namespace Shader::Backend::SPIRV
