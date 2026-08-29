// SPDX-FileCopyrightText: Copyright 2021 yuzu Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <array>
#include <cstdlib>
#include <ranges>

#include "shader_recompiler/backend/spirv/emit_spirv_instructions.h"
#include "shader_recompiler/backend/spirv/spirv_emit_context.h"
#include "shader_recompiler/dreams_compat.h"

namespace Shader::Backend::SPIRV {

namespace {
using PointerType = EmitContext::PointerType;
using PointerSize = EmitContext::PointerSize;

std::pair<Id, Id> AtomicArgs(EmitContext& ctx) {
    const Id scope{ctx.ConstU32(static_cast<u32>(spv::Scope::Device))};
    const Id semantics{ctx.u32_zero_value};
    return {scope, semantics};
}

enum class AtomicImageType {
    U32,
    S32,
    F32,
};

AtomicImageType GetAtomicImageType(EmitContext& ctx,
                                   const EmitContext::TextureDefinition& texture) {
    if (texture.data_types == &ctx.U32) {
        return AtomicImageType::U32;
    }
    if (texture.data_types == &ctx.S32) {
        return AtomicImageType::S32;
    }
    return AtomicImageType::F32;
}

Id ImageAtomicReadF32(EmitContext& ctx, const EmitContext::TextureDefinition& texture, Id coords) {
    const Id image{ctx.OpLoad(texture.image_type, texture.id)};
    const Id texel{ctx.OpImageRead(ctx.F32[4], image, coords)};
    return ctx.OpCompositeExtract(ctx.F32[1], texel, 0);
}

Id SharedAtomicU32(EmitContext& ctx, Id offset, Id value,
                   Id (Sirit::Module::*atomic_func)(Id, Id, Id, Id, Id)) {
    const Id shift_id{ctx.ConstU32(2U)};
    const Id index{ctx.OpShiftRightLogical(ctx.U32[1], offset, shift_id)};
    const Id pointer{ctx.EmitSharedMemoryAccess(ctx.shared_u32, ctx.shared_memory_u32, index)};
    const auto [scope, semantics]{AtomicArgs(ctx)};
    return (ctx.*atomic_func)(ctx.U32[1], pointer, scope, semantics, value);
}

Id SharedAtomicU32IncDec(EmitContext& ctx, Id offset,
                         Id (Sirit::Module::*atomic_func)(Id, Id, Id, Id)) {
    const Id shift_id{ctx.ConstU32(2U)};
    const Id index{ctx.OpShiftRightLogical(ctx.U32[1], offset, shift_id)};
    const Id pointer{ctx.EmitSharedMemoryAccess(ctx.shared_u32, ctx.shared_memory_u32, index)};
    const auto [scope, semantics]{AtomicArgs(ctx)};
    return (ctx.*atomic_func)(ctx.U32[1], pointer, scope, semantics);
}

Id SharedAtomicU64(EmitContext& ctx, Id offset, Id value,
                   Id (Sirit::Module::*atomic_func)(Id, Id, Id, Id, Id)) {
    const Id shift_id{ctx.ConstU32(3U)};
    const Id index{ctx.OpShiftRightLogical(ctx.U32[1], offset, shift_id)};
    const Id pointer{ctx.EmitSharedMemoryAccess(ctx.shared_u64, ctx.shared_memory_u64, index)};
    const auto [scope, semantics]{AtomicArgs(ctx)};
    return (ctx.*atomic_func)(ctx.U64, pointer, scope, semantics, value);
}

Id SharedAtomicU64IncDec(EmitContext& ctx, Id offset,
                         Id (Sirit::Module::*atomic_func)(Id, Id, Id, Id)) {
    const Id shift_id{ctx.ConstU32(3U)};
    const Id index{ctx.OpShiftRightLogical(ctx.U32[1], offset, shift_id)};
    const Id pointer{ctx.EmitSharedMemoryAccess(ctx.shared_u64, ctx.shared_memory_u64, index)};
    const auto [scope, semantics]{AtomicArgs(ctx)};
    return (ctx.*atomic_func)(ctx.U64, pointer, scope, semantics);
}

template <bool is_float = false>
Id BufferAtomicU32(EmitContext& ctx, IR::Inst* inst, u32 handle, Id address, Id value,
                   Id (Sirit::Module::*atomic_func)(Id, Id, Id, Id, Id)) {
    const auto& buffer = ctx.buffers[handle];
    const Id type = is_float ? ctx.F32[1] : ctx.U32[1];
    if (const Id offset = buffer.Offset(PointerSize::B32); Sirit::ValidId(offset)) {
        address = ctx.OpIAdd(ctx.U32[1], address, offset);
    }
    const auto [id, pointer_type] = buffer.Alias(is_float ? PointerType::F32 : PointerType::U32);
    const Id ptr = ctx.OpAccessChain(pointer_type, id, ctx.u32_zero_value, address);
    const auto [scope, semantics]{AtomicArgs(ctx)};
    return (ctx.*atomic_func)(type, ptr, scope, semantics, value);
}

Id BufferAtomicIAddSubgroupLeader(EmitContext& ctx, IR::Inst* inst, u32 handle, Id address,
                                  Id value) {
    const Id subgroup_scope = ctx.ConstU32(static_cast<u32>(spv::Scope::Subgroup));
    const Id active_mask = ctx.OpGroupNonUniformBallot(ctx.U32[4], subgroup_scope, ctx.true_value);
    const Id first_active =
        ctx.OpGroupNonUniformBallotFindLSB(ctx.U32[1], subgroup_scope, active_mask);
    const Id lane_id = ctx.OpLoad(ctx.U32[1], ctx.subgroup_local_invocation_id);
    const Id is_leader = ctx.OpIEqual(ctx.U1[1], lane_id, first_active);

    // Keep every active invocation on the same control-flow path through the atomic and
    // broadcast. Core Vulkan does not guarantee subgroup reconvergence at a selection merge
    // reached from divergent control flow, which is where GDS append is commonly emitted.
    const Id delta = ctx.OpSelect(ctx.U32[1], is_leader, value, ctx.u32_zero_value);
    const Id previous =
        BufferAtomicU32(ctx, inst, handle, address, delta, &Sirit::Module::OpAtomicIAdd);
    return ctx.OpGroupNonUniformBroadcastFirst(ctx.U32[1], subgroup_scope, previous);
}

Id BufferAtomicIncSubgroupCompact(EmitContext& ctx, IR::Inst* inst, u32 handle, Id address) {
    const Id subgroup_scope = ctx.ConstU32(static_cast<u32>(spv::Scope::Subgroup));
    const Id active_mask = ctx.OpGroupNonUniformBallot(ctx.U32[4], subgroup_scope, ctx.true_value);
    Id active_count = ctx.u32_zero_value;
    for (u32 component = 0; component < 4; ++component) {
        const Id mask_component = ctx.OpCompositeExtract(ctx.U32[1], active_mask, component);
        active_count =
            ctx.OpIAdd(ctx.U32[1], active_count, ctx.OpBitCount(ctx.U32[1], mask_component));
    }
    return BufferAtomicIAddSubgroupLeader(ctx, inst, handle, address, active_count);
}

Id BufferAtomicU32IncDec(EmitContext& ctx, IR::Inst* inst, u32 handle, Id address,
                         Id (Sirit::Module::*atomic_func)(Id, Id, Id, Id)) {
    const auto& buffer = ctx.buffers[handle];
    if (const Id offset = buffer.Offset(PointerSize::B32); Sirit::ValidId(offset)) {
        address = ctx.OpIAdd(ctx.U32[1], address, offset);
    }
    const auto [id, pointer_type] = buffer.Alias(PointerType::U32);
    const Id ptr = ctx.OpAccessChain(pointer_type, id, ctx.u32_zero_value, address);
    const auto [scope, semantics]{AtomicArgs(ctx)};
    return (ctx.*atomic_func)(ctx.U32[1], ptr, scope, semantics);
}

Id BufferAtomicU32CmpSwap(EmitContext& ctx, IR::Inst* inst, u32 handle, Id address, Id value,
                          Id cmp_value,
                          Id (Sirit::Module::*atomic_func)(Id, Id, Id, Id, Id, Id, Id)) {
    const auto& buffer = ctx.buffers[handle];
    if (const Id offset = buffer.Offset(PointerSize::B32); Sirit::ValidId(offset)) {
        address = ctx.OpIAdd(ctx.U32[1], address, offset);
    }
    const auto [id, pointer_type] = buffer.Alias(PointerType::U32);
    const Id ptr = ctx.OpAccessChain(pointer_type, id, ctx.u32_zero_value, address);
    const auto [scope, semantics]{AtomicArgs(ctx)};
    return (ctx.*atomic_func)(ctx.U32[1], ptr, scope, semantics, semantics, value, cmp_value);
}

Id BufferAtomicU64(EmitContext& ctx, IR::Inst* inst, u32 handle, Id address, Id value,
                   Id (Sirit::Module::*atomic_func)(Id, Id, Id, Id, Id)) {
    const auto& buffer = ctx.buffers[handle];
    if (const Id offset = buffer.Offset(PointerSize::B64); Sirit::ValidId(offset)) {
        address = ctx.OpIAdd(ctx.U32[1], address, offset);
    }
    const auto [id, pointer_type] = buffer.Alias(PointerType::U64);
    const Id ptr = ctx.OpAccessChain(pointer_type, id, ctx.u32_zero_value, address);
    const auto [scope, semantics]{AtomicArgs(ctx)};
    return (ctx.*atomic_func)(ctx.U64, ptr, scope, semantics, value);
}

Id ImageAtomicU32(EmitContext& ctx, IR::Inst* inst, u32 handle, Id coords, Id value,
                  Id (Sirit::Module::*atomic_func)(Id, Id, Id, Id, Id)) {
    const auto& texture = ctx.images[handle & 0xFFFF];
    const auto image_type = GetAtomicImageType(ctx, texture);
    if (image_type == AtomicImageType::F32) {
        LOG_WARNING(Render, "Unsupported integer atomic on non-integer image");
        return ctx.u32_zero_value;
    }
    const Id result_type = image_type == AtomicImageType::S32 ? ctx.S32[1] : ctx.U32[1];
    const Id pointer_type = image_type == AtomicImageType::S32 ? ctx.image_s32 : ctx.image_u32;
    const Id typed_value =
        image_type == AtomicImageType::S32 ? ctx.OpBitcast(ctx.S32[1], value) : value;
    const Id pointer{ctx.OpImageTexelPointer(pointer_type, texture.id, coords, ctx.ConstU32(0U))};
    const auto [scope, semantics]{AtomicArgs(ctx)};
    const Id result{(ctx.*atomic_func)(result_type, pointer, scope, semantics, typed_value)};
    return image_type == AtomicImageType::S32 ? ctx.OpBitcast(ctx.U32[1], result) : result;
}

Id ImageAtomicF32(EmitContext& ctx, IR::Inst* inst, u32 handle, Id coords, Id value,
                  Id (Sirit::Module::*atomic_func)(Id, Id, Id, Id, Id)) {
    const auto& texture = ctx.images[handle & 0xFFFF];
    if (GetAtomicImageType(ctx, texture) != AtomicImageType::F32) {
        LOG_WARNING(Render, "Unsupported float atomic on non-float image");
        return ctx.f32_zero_value;
    }
    const Id pointer{ctx.OpImageTexelPointer(ctx.image_f32, texture.id, coords, ctx.ConstU32(0U))};
    const auto [scope, semantics]{AtomicArgs(ctx)};
    return (ctx.*atomic_func)(ctx.F32[1], pointer, scope, semantics, value);
}

Id ImageAtomicU32CmpSwap(EmitContext& ctx, IR::Inst* inst, u32 handle, Id coords, Id value,
                         Id cmp_value,
                         Id (Sirit::Module::*atomic_func)(Id, Id, Id, Id, Id, Id, Id)) {
    const auto& texture = ctx.images[handle & 0xFFFF];
    const auto image_type = GetAtomicImageType(ctx, texture);
    if (image_type == AtomicImageType::F32) {
        LOG_WARNING(Render, "Unsupported integer atomic compare-swap on non-integer image");
        return ctx.u32_zero_value;
    }
    const Id result_type = image_type == AtomicImageType::S32 ? ctx.S32[1] : ctx.U32[1];
    const Id pointer_type = image_type == AtomicImageType::S32 ? ctx.image_s32 : ctx.image_u32;
    const Id typed_value =
        image_type == AtomicImageType::S32 ? ctx.OpBitcast(ctx.S32[1], value) : value;
    const Id typed_cmp_value =
        image_type == AtomicImageType::S32 ? ctx.OpBitcast(ctx.S32[1], cmp_value) : cmp_value;
    const Id pointer{ctx.OpImageTexelPointer(pointer_type, texture.id, coords, ctx.ConstU32(0U))};
    const auto [scope, semantics]{AtomicArgs(ctx)};
    const Id result{(ctx.*atomic_func)(result_type, pointer, scope, semantics, semantics,
                                       typed_value, typed_cmp_value)};
    return image_type == AtomicImageType::S32 ? ctx.OpBitcast(ctx.U32[1], result) : result;
}
} // Anonymous namespace

Id EmitSharedAtomicIAdd32(EmitContext& ctx, Id offset, Id value) {
    return SharedAtomicU32(ctx, offset, value, &Sirit::Module::OpAtomicIAdd);
}

Id EmitSharedAtomicIAdd64(EmitContext& ctx, Id offset, Id value) {
    return SharedAtomicU64(ctx, offset, value, &Sirit::Module::OpAtomicIAdd);
}

Id EmitSharedAtomicUMax32(EmitContext& ctx, Id offset, Id value) {
    return SharedAtomicU32(ctx, offset, value, &Sirit::Module::OpAtomicUMax);
}

Id EmitSharedAtomicUMax64(EmitContext& ctx, Id offset, Id value) {
    return SharedAtomicU64(ctx, offset, value, &Sirit::Module::OpAtomicUMax);
}

Id EmitSharedAtomicSMax32(EmitContext& ctx, Id offset, Id value) {
    return SharedAtomicU32(ctx, offset, value, &Sirit::Module::OpAtomicSMax);
}

Id EmitSharedAtomicSMax64(EmitContext& ctx, Id offset, Id value) {
    return SharedAtomicU64(ctx, offset, value, &Sirit::Module::OpAtomicSMax);
}

Id EmitSharedAtomicUMin32(EmitContext& ctx, Id offset, Id value) {
    return SharedAtomicU32(ctx, offset, value, &Sirit::Module::OpAtomicUMin);
}

Id EmitSharedAtomicUMin64(EmitContext& ctx, Id offset, Id value) {
    return SharedAtomicU64(ctx, offset, value, &Sirit::Module::OpAtomicUMin);
}

Id EmitSharedAtomicSMin32(EmitContext& ctx, Id offset, Id value) {
    return SharedAtomicU32(ctx, offset, value, &Sirit::Module::OpAtomicSMin);
}

Id EmitSharedAtomicSMin64(EmitContext& ctx, Id offset, Id value) {
    return SharedAtomicU64(ctx, offset, value, &Sirit::Module::OpAtomicSMin);
}

Id EmitSharedAtomicAnd32(EmitContext& ctx, Id offset, Id value) {
    return SharedAtomicU32(ctx, offset, value, &Sirit::Module::OpAtomicAnd);
}

Id EmitSharedAtomicAnd64(EmitContext& ctx, Id offset, Id value) {
    return SharedAtomicU64(ctx, offset, value, &Sirit::Module::OpAtomicAnd);
}

Id EmitSharedAtomicOr32(EmitContext& ctx, Id offset, Id value) {
    return SharedAtomicU32(ctx, offset, value, &Sirit::Module::OpAtomicOr);
}

Id EmitSharedAtomicOr64(EmitContext& ctx, Id offset, Id value) {
    return SharedAtomicU64(ctx, offset, value, &Sirit::Module::OpAtomicOr);
}

Id EmitSharedAtomicXor32(EmitContext& ctx, Id offset, Id value) {
    return SharedAtomicU32(ctx, offset, value, &Sirit::Module::OpAtomicXor);
}

Id EmitSharedAtomicXor64(EmitContext& ctx, Id offset, Id value) {
    return SharedAtomicU64(ctx, offset, value, &Sirit::Module::OpAtomicXor);
}

Id EmitSharedAtomicISub32(EmitContext& ctx, Id offset, Id value) {
    return SharedAtomicU32(ctx, offset, value, &Sirit::Module::OpAtomicISub);
}

Id EmitSharedAtomicISub64(EmitContext& ctx, Id offset, Id value) {
    return SharedAtomicU64(ctx, offset, value, &Sirit::Module::OpAtomicISub);
}

Id EmitSharedAtomicInc32(EmitContext& ctx, Id offset) {
    return SharedAtomicU32IncDec(ctx, offset, &Sirit::Module::OpAtomicIIncrement);
}

Id EmitSharedAtomicInc64(EmitContext& ctx, Id offset) {
    return SharedAtomicU64IncDec(ctx, offset, &Sirit::Module::OpAtomicIIncrement);
}

Id EmitSharedAtomicDec32(EmitContext& ctx, Id offset) {
    return SharedAtomicU32IncDec(ctx, offset, &Sirit::Module::OpAtomicIDecrement);
}

Id EmitSharedAtomicDec64(EmitContext& ctx, Id offset) {
    return SharedAtomicU64IncDec(ctx, offset, &Sirit::Module::OpAtomicIDecrement);
}

Id EmitBufferAtomicIAdd32(EmitContext& ctx, IR::Inst* inst, u32 handle, Id address, Id value) {
    const bool is_dreams_queue_producer = ctx.info.pgm_hash == DreamsCompat::QueueProducerShader ||
                                          ctx.info.pgm_hash == DreamsCompat::QueueProducerShaderAlt;
    if (is_dreams_queue_producer && ctx.buffers[handle].buffer_type == BufferType::GdsBuffer) {
        // Dreams compacts a subgroup by adding its ballot population once, then broadcasting the
        // returned queue base. Executing the translated atomic in every host lane multiplies the
        // queue count by 64 and leaves almost every queue record unwritten.
        return BufferAtomicIAddSubgroupLeader(ctx, inst, handle, address, value);
    }
    return BufferAtomicU32(ctx, inst, handle, address, value, &Sirit::Module::OpAtomicIAdd);
}

Id EmitBufferAtomicIAdd64(EmitContext& ctx, IR::Inst* inst, u32 handle, Id address, Id value) {
    return BufferAtomicU64(ctx, inst, handle, address, value, &Sirit::Module::OpAtomicIAdd);
}

Id EmitBufferAtomicISub32(EmitContext& ctx, IR::Inst* inst, u32 handle, Id address, Id value) {
    return BufferAtomicU32(ctx, inst, handle, address, value, &Sirit::Module::OpAtomicISub);
}

Id EmitBufferAtomicSMin32(EmitContext& ctx, IR::Inst* inst, u32 handle, Id address, Id value) {
    return BufferAtomicU32(ctx, inst, handle, address, value, &Sirit::Module::OpAtomicSMin);
}

Id EmitBufferAtomicSMin64(EmitContext& ctx, IR::Inst* inst, u32 handle, Id address, Id value) {
    return BufferAtomicU64(ctx, inst, handle, address, value, &Sirit::Module::OpAtomicSMin);
}

Id EmitBufferAtomicUMin32(EmitContext& ctx, IR::Inst* inst, u32 handle, Id address, Id value) {
    return BufferAtomicU32(ctx, inst, handle, address, value, &Sirit::Module::OpAtomicUMin);
}

Id EmitBufferAtomicUMin64(EmitContext& ctx, IR::Inst* inst, u32 handle, Id address, Id value) {
    return BufferAtomicU64(ctx, inst, handle, address, value, &Sirit::Module::OpAtomicUMin);
}

Id EmitBufferAtomicFMin32(EmitContext& ctx, IR::Inst* inst, u32 handle, Id address, Id value) {
    if (ctx.profile.supports_buffer_fp32_atomic_min_max) {
        return BufferAtomicU32<true>(ctx, inst, handle, address, value,
                                     &Sirit::Module::OpAtomicFMin);
    }

    const auto u32_value = ctx.OpBitcast(ctx.U32[1], value);
    // OpSelect requires a bool condition; produce one by comparing the sign bit to 0.
    const auto sign_bit_set = ctx.OpINotEqual(
        ctx.U1[1],
        ctx.OpBitFieldUExtract(ctx.U32[1], u32_value, ctx.ConstU32(31u), ctx.ConstU32(1u)),
        ctx.u32_zero_value);

    // FIXME this needs control flow because it currently executes both atomics
    const auto result = ctx.OpSelect(
        ctx.F32[1], sign_bit_set,
        EmitBitCastF32U32(ctx, EmitBufferAtomicUMax32(ctx, inst, handle, address, u32_value)),
        EmitBitCastF32U32(ctx, EmitBufferAtomicSMin32(ctx, inst, handle, address, u32_value)));

    return result;
}

Id EmitBufferAtomicSMax32(EmitContext& ctx, IR::Inst* inst, u32 handle, Id address, Id value) {
    return BufferAtomicU32(ctx, inst, handle, address, value, &Sirit::Module::OpAtomicSMax);
}

Id EmitBufferAtomicSMax64(EmitContext& ctx, IR::Inst* inst, u32 handle, Id address, Id value) {
    return BufferAtomicU64(ctx, inst, handle, address, value, &Sirit::Module::OpAtomicSMax);
}

Id EmitBufferAtomicUMax32(EmitContext& ctx, IR::Inst* inst, u32 handle, Id address, Id value) {
    return BufferAtomicU32(ctx, inst, handle, address, value, &Sirit::Module::OpAtomicUMax);
}

Id EmitBufferAtomicUMax64(EmitContext& ctx, IR::Inst* inst, u32 handle, Id address, Id value) {
    return BufferAtomicU64(ctx, inst, handle, address, value, &Sirit::Module::OpAtomicUMax);
}

Id EmitBufferAtomicFMax32(EmitContext& ctx, IR::Inst* inst, u32 handle, Id address, Id value) {
    if (ctx.profile.supports_buffer_fp32_atomic_min_max) {
        return BufferAtomicU32<true>(ctx, inst, handle, address, value,
                                     &Sirit::Module::OpAtomicFMax);
    }

    const auto u32_value = ctx.OpBitcast(ctx.U32[1], value);
    // OpSelect requires a bool condition; produce one by comparing the sign bit to 0.
    const auto sign_bit_set = ctx.OpINotEqual(
        ctx.U1[1],
        ctx.OpBitFieldUExtract(ctx.U32[1], u32_value, ctx.ConstU32(31u), ctx.ConstU32(1u)),
        ctx.u32_zero_value);

    // FIXME this needs control flow because it currently executes both atomics
    const auto result = ctx.OpSelect(
        ctx.F32[1], sign_bit_set,
        EmitBitCastF32U32(ctx, EmitBufferAtomicUMin32(ctx, inst, handle, address, u32_value)),
        EmitBitCastF32U32(ctx, EmitBufferAtomicSMax32(ctx, inst, handle, address, u32_value)));

    return result;
}

Id EmitBufferAtomicInc32(EmitContext& ctx, IR::Inst* inst, u32 handle, Id address) {
    const bool is_dreams_sprite_cull = DreamsCompat::IsSpriteCullShader(ctx.info.pgm_hash);
    const bool is_gds_append = inst->Flags<IR::BufferInstInfo>().is_gds_append;
    if ((is_gds_append || is_dreams_sprite_cull) &&
        ctx.buffers[handle].buffer_type == BufferType::GdsBuffer) {
        // These shaders add a lane prefix to the returned counter base. Reserve the active
        // subgroup once; incrementing from every host lane double-counts and leaves holes.
        return BufferAtomicIncSubgroupCompact(ctx, inst, handle, address);
    }
    return BufferAtomicU32IncDec(ctx, inst, handle, address, &Sirit::Module::OpAtomicIIncrement);
}

Id EmitBufferAtomicDec32(EmitContext& ctx, IR::Inst* inst, u32 handle, Id address) {
    return BufferAtomicU32IncDec(ctx, inst, handle, address, &Sirit::Module::OpAtomicIDecrement);
}

Id EmitBufferAtomicAnd32(EmitContext& ctx, IR::Inst* inst, u32 handle, Id address, Id value) {
    return BufferAtomicU32(ctx, inst, handle, address, value, &Sirit::Module::OpAtomicAnd);
}

Id EmitBufferAtomicOr32(EmitContext& ctx, IR::Inst* inst, u32 handle, Id address, Id value) {
    return BufferAtomicU32(ctx, inst, handle, address, value, &Sirit::Module::OpAtomicOr);
}

Id EmitBufferAtomicXor32(EmitContext& ctx, IR::Inst* inst, u32 handle, Id address, Id value) {
    return BufferAtomicU32(ctx, inst, handle, address, value, &Sirit::Module::OpAtomicXor);
}

Id EmitBufferAtomicSwap32(EmitContext& ctx, IR::Inst* inst, u32 handle, Id address, Id value) {
    return BufferAtomicU32(ctx, inst, handle, address, value, &Sirit::Module::OpAtomicExchange);
}

Id EmitBufferAtomicCmpSwap32(EmitContext& ctx, IR::Inst* inst, u32 handle, Id address, Id value,
                             Id cmp_value) {
    return BufferAtomicU32CmpSwap(ctx, inst, handle, address, value, cmp_value,
                                  &Sirit::Module::OpAtomicCompareExchange);
}

Id EmitBufferAtomicFCmpSwap32(EmitContext& ctx, IR::Inst* inst, u32 handle, Id address, Id value,
                              Id cmp_value) {
    const auto u32_value = ctx.OpBitcast(ctx.U32[1], value);
    const auto u32_cmp = ctx.OpBitcast(ctx.U32[1], cmp_value);
    const auto result = BufferAtomicU32CmpSwap(ctx, inst, handle, address, u32_value, u32_cmp,
                                               &Sirit::Module::OpAtomicCompareExchange);
    return ctx.OpBitcast(ctx.F32[1], result);
}

Id EmitImageAtomicIAdd32(EmitContext& ctx, IR::Inst* inst, u32 handle, Id coords, Id value) {
    return ImageAtomicU32(ctx, inst, handle, coords, value, &Sirit::Module::OpAtomicIAdd);
}

Id EmitImageAtomicSMin32(EmitContext& ctx, IR::Inst* inst, u32 handle, Id coords, Id value) {
    return ImageAtomicU32(ctx, inst, handle, coords, value, &Sirit::Module::OpAtomicSMin);
}

Id EmitImageAtomicUMin32(EmitContext& ctx, IR::Inst* inst, u32 handle, Id coords, Id value) {
    return ImageAtomicU32(ctx, inst, handle, coords, value, &Sirit::Module::OpAtomicUMin);
}

Id EmitImageAtomicSMax32(EmitContext& ctx, IR::Inst* inst, u32 handle, Id coords, Id value) {
    return ImageAtomicU32(ctx, inst, handle, coords, value, &Sirit::Module::OpAtomicSMax);
}

Id EmitImageAtomicUMax32(EmitContext& ctx, IR::Inst* inst, u32 handle, Id coords, Id value) {
    return ImageAtomicU32(ctx, inst, handle, coords, value, &Sirit::Module::OpAtomicUMax);
}

Id EmitImageAtomicFMax32(EmitContext& ctx, IR::Inst* inst, u32 handle, Id coords, Id value) {
    if (ctx.profile.supports_image_fp32_atomic_min_max) {
        return ImageAtomicF32(ctx, inst, handle, coords, value, &Sirit::Module::OpAtomicFMax);
    }
    const auto& texture = ctx.images[handle & 0xFFFF];
    if (GetAtomicImageType(ctx, texture) == AtomicImageType::F32) {
        LOG_WARNING(Render, "Falling back image float atomic max to a non-atomic read");
        return ImageAtomicReadF32(ctx, texture, coords);
    }

    const auto u32_value = ctx.OpBitcast(ctx.U32[1], value);
    // OpSelect requires a bool condition; produce one by comparing the sign bit to 0.
    const auto sign_bit_set = ctx.OpINotEqual(
        ctx.U1[1],
        ctx.OpBitFieldUExtract(ctx.U32[1], u32_value, ctx.ConstU32(31u), ctx.ConstU32(1u)),
        ctx.u32_zero_value);

    const auto result = ctx.OpSelect(
        ctx.F32[1], sign_bit_set,
        EmitBitCastF32U32(ctx, EmitImageAtomicUMin32(ctx, inst, handle, coords, u32_value)),
        EmitBitCastF32U32(ctx, EmitImageAtomicSMax32(ctx, inst, handle, coords, u32_value)));

    return result;
}

Id EmitImageAtomicFMin32(EmitContext& ctx, IR::Inst* inst, u32 handle, Id coords, Id value) {
    if (ctx.profile.supports_image_fp32_atomic_min_max) {
        return ImageAtomicF32(ctx, inst, handle, coords, value, &Sirit::Module::OpAtomicFMin);
    }
    const auto& texture = ctx.images[handle & 0xFFFF];
    if (GetAtomicImageType(ctx, texture) == AtomicImageType::F32) {
        LOG_WARNING(Render, "Falling back image float atomic min to a non-atomic read");
        return ImageAtomicReadF32(ctx, texture, coords);
    }

    const auto u32_value = ctx.OpBitcast(ctx.U32[1], value);
    // OpSelect requires a bool condition; produce one by comparing the sign bit to 0.
    const auto sign_bit_set = ctx.OpINotEqual(
        ctx.U1[1],
        ctx.OpBitFieldUExtract(ctx.U32[1], u32_value, ctx.ConstU32(31u), ctx.ConstU32(1u)),
        ctx.u32_zero_value);

    const auto result = ctx.OpSelect(
        ctx.F32[1], sign_bit_set,
        EmitBitCastF32U32(ctx, EmitImageAtomicUMax32(ctx, inst, handle, coords, u32_value)),
        EmitBitCastF32U32(ctx, EmitImageAtomicSMin32(ctx, inst, handle, coords, u32_value)));

    return result;
}

Id EmitImageAtomicInc32(EmitContext&, IR::Inst*, u32, Id, Id) {
    UNREACHABLE_MSG("SPIR-V Instruction");
}

Id EmitImageAtomicDec32(EmitContext&, IR::Inst*, u32, Id, Id) {
    UNREACHABLE_MSG("SPIR-V Instruction");
}

Id EmitImageAtomicAnd32(EmitContext& ctx, IR::Inst* inst, u32 handle, Id coords, Id value) {
    return ImageAtomicU32(ctx, inst, handle, coords, value, &Sirit::Module::OpAtomicAnd);
}

Id EmitImageAtomicOr32(EmitContext& ctx, IR::Inst* inst, u32 handle, Id coords, Id value) {
    return ImageAtomicU32(ctx, inst, handle, coords, value, &Sirit::Module::OpAtomicOr);
}

Id EmitImageAtomicXor32(EmitContext& ctx, IR::Inst* inst, u32 handle, Id coords, Id value) {
    return ImageAtomicU32(ctx, inst, handle, coords, value, &Sirit::Module::OpAtomicXor);
}

Id EmitImageAtomicExchange32(EmitContext& ctx, IR::Inst* inst, u32 handle, Id coords, Id value) {
    return ImageAtomicU32(ctx, inst, handle, coords, value, &Sirit::Module::OpAtomicExchange);
}

Id EmitImageAtomicCmpSwap32(EmitContext& ctx, IR::Inst* inst, u32 handle, Id coords, Id value,
                            Id cmp_value) {
    return ImageAtomicU32CmpSwap(ctx, inst, handle, coords, value, cmp_value,
                                 &Sirit::Module::OpAtomicCompareExchange);
}

Id EmitDataAppend(EmitContext& ctx, u32 gds_addr, u32 binding) {
    UNREACHABLE_MSG("SPIR-V Instruction");
}

Id EmitDataConsume(EmitContext& ctx, u32 gds_addr, u32 binding) {
    UNREACHABLE_MSG("SPIR-V Instruction");
}

void EmitDreamsSculptCheckpoint(EmitContext& ctx, IR::Inst* inst, Id state0, Id state1, Id exec) {
    ASSERT_MSG(ctx.info.pgm_hash == DreamsCompat::SculptVolumeWriterShader,
               "Sculpt checkpoint emitted for unexpected shader {:#x}", ctx.info.pgm_hash);

    const auto gds_it = std::ranges::find(ctx.buffers, BufferType::GdsBuffer,
                                          &EmitContext::BufferDefinition::buffer_type);
    ASSERT_MSG(gds_it != ctx.buffers.end(), "Dreams 84aa checkpoint has no GDS buffer");
    const auto& buffer = *gds_it;
    const auto [buffer_id, pointer_type] = buffer.Alias(PointerType::U32);
    const auto get_pointer = [&](Id address) {
        if (const Id offset = buffer.Offset(PointerSize::B32); Sirit::ValidId(offset)) {
            address = ctx.OpIAdd(ctx.U32[1], address, offset);
        }
        return ctx.OpAccessChain(pointer_type, buffer_id, ctx.u32_zero_value, address);
    };

    ASSERT_MSG(Sirit::ValidId(ctx.dreams_ordered_phase),
               "Dreams 84aa checkpoint has no phase specialization constant");
    const Id phase = ctx.dreams_ordered_phase;
    const Id is_collect =
        ctx.OpIEqual(ctx.U1[1], phase, ctx.ConstU32(DreamsCompat::OrderedPhaseCollect));

    const Id workgroup_x =
        ctx.OpCompositeExtract(ctx.U32[1], ctx.OpLoad(ctx.U32[3], ctx.workgroup_id), 0);
    const Id local_x =
        ctx.OpCompositeExtract(ctx.U32[1], ctx.OpLoad(ctx.U32[3], ctx.local_invocation_id), 0);
    const Id workgroup_state_base =
        ctx.OpIAdd(ctx.U32[1], ctx.ConstU32(DreamsCompat::SculptOrderedStateBaseDword),
                   ctx.OpIMul(ctx.U32[1], workgroup_x,
                              ctx.ConstU32(DreamsCompat::SculptOrderedStateDwordsPerWorkgroup)));

    std::array<Id, DreamsCompat::SculptOrderedStateValueCount + 1> original{};
    for (u32 index = 0; index < 4; ++index) {
        original[index] = ctx.OpCompositeExtract(ctx.U32[1], state0, index);
    }
    for (u32 index = 0; index < 3; ++index) {
        original[index + 4] = ctx.OpCompositeExtract(ctx.U32[1], state1, index);
    }
    original[7] = ctx.OpSelect(ctx.U32[1], exec, ctx.u32_one_value, ctx.u32_zero_value);

    for (u32 vector = 0; vector < 2; ++vector) {
        IR::Inst* const composite = inst->Arg(vector).InstRecursive();
        const u32 components = vector == 0 ? 4 : 3;
        ASSERT_MSG(composite->GetOpcode() == (vector == 0 ? IR::Opcode::CompositeConstructU32x4
                                                          : IR::Opcode::CompositeConstructU32x3),
                   "Dreams 84aa checkpoint state vector has unexpected opcode");
        for (u32 component = 0; component < components; ++component) {
            ctx.dreams_sculpt_state_insts[vector * 4 + component] =
                composite->Arg(component).InstRecursive();
        }
    }
    ctx.dreams_sculpt_state_insts[7] = inst->Arg(2).InstRecursive();
    ctx.dreams_sculpt_normal_values = original;
    ctx.dreams_sculpt_normal_values[7] = exec;
    for (u32 index = 0; index < ctx.dreams_sculpt_state_insts.size(); ++index) {
        ASSERT_MSG(ctx.dreams_sculpt_state_insts[index] != nullptr &&
                       ctx.dreams_sculpt_state_insts[index]->GetOpcode() == IR::Opcode::Phi,
                   "Dreams 84aa checkpoint value {} is not a phi", index);
    }

    const Id store_label = ctx.OpLabel();
    const Id store_merge_label = ctx.OpLabel();
    ctx.OpSelectionMerge(store_merge_label, spv::SelectionControlMask::MaskNone);
    ctx.OpBranchConditional(is_collect, store_label, store_merge_label);
    ctx.AddLabel(store_label);
    const auto store_state = [&](u32 index) {
        const Id component_base = ctx.OpIAdd(
            ctx.U32[1], workgroup_state_base,
            ctx.ConstU32(DreamsCompat::SculptOrderedStateComponentOffset(index)));
        const Id address = ctx.OpIAdd(ctx.U32[1], component_base, local_x);
        ctx.OpStore(get_pointer(address), original[index]);
    };

    // State 0..3 feeds an output guarded by LocalInvocationId.x < 32. Its upper-half values are
    // known zero and never consumed, so checkpoint only the lanes that Replay can observe.
    const Id lower_half_store_label = ctx.OpLabel();
    const Id full_state_store_label = ctx.OpLabel();
    const Id is_lower_half = ctx.OpULessThan(
        ctx.U1[1], local_x, ctx.ConstU32(DreamsCompat::SculptOrderedLowerHalfLanes));
    ctx.OpSelectionMerge(full_state_store_label, spv::SelectionControlMask::MaskNone);
    ctx.OpBranchConditional(is_lower_half, lower_half_store_label, full_state_store_label);

    ctx.AddLabel(lower_half_store_label);
    for (u32 index = 0; index < DreamsCompat::SculptOrderedLowerHalfStateComponents; ++index) {
        store_state(index);
    }
    ctx.OpBranch(full_state_store_label);

    ctx.AddLabel(full_state_store_label);
    for (u32 index = DreamsCompat::SculptOrderedLowerHalfStateComponents;
         index < DreamsCompat::SculptOrderedStateValueCount; ++index) {
        store_state(index);
    }
    ctx.OpBranch(store_merge_label);
    ctx.AddLabel(store_merge_label);
}

Id EmitDataOrderedCount(EmitContext& ctx, Id gds_index, Id value, Id ordered_token,
                        u32 packed_control) {
    const u32 handle = packed_control >> 16;
    const u32 control = packed_control & 0xffff;

    const auto& buffer = ctx.buffers[handle];
    const auto [buffer_id, pointer_type] = buffer.Alias(PointerType::U32);
    const auto get_pointer = [&](Id address) {
        if (const Id offset = buffer.Offset(PointerSize::B32); Sirit::ValidId(offset)) {
            address = ctx.OpIAdd(ctx.U32[1], address, offset);
        }
        return ctx.OpAccessChain(pointer_type, buffer_id, ctx.u32_zero_value, address);
    };
    const Id counter_ptr = get_pointer(gds_index);
    const Id device_scope = ctx.ConstU32(static_cast<u32>(spv::Scope::Device));
    const auto acquire_release =
        spv::MemorySemanticsMask::AcquireRelease | spv::MemorySemanticsMask::UniformMemory;
    const Id acquire_release_semantics = ctx.ConstU32(static_cast<u32>(acquire_release));
    const auto acquire =
        spv::MemorySemanticsMask::Acquire | spv::MemorySemanticsMask::UniformMemory;
    const Id acquire_semantics = ctx.ConstU32(static_cast<u32>(acquire));

    const Id subgroup_scope = ctx.ConstU32(static_cast<u32>(spv::Scope::Subgroup));
    const Id active_mask = ctx.OpGroupNonUniformBallot(ctx.U32[4], subgroup_scope, ctx.true_value);
    const Id first_active =
        ctx.OpGroupNonUniformBallotFindLSB(ctx.U32[1], subgroup_scope, active_mask);
    const Id lane_id = ctx.OpLoad(ctx.U32[1], ctx.subgroup_local_invocation_id);
    const Id is_leader = ctx.OpIEqual(ctx.U1[1], lane_id, first_active);

    if (ctx.info.pgm_hash == DreamsCompat::QueueProducerShader) {
        // The aligned Dreams chain dispatches 2bf as exactly one 64-lane workgroup/wave. There is
        // no second wave whose host arrival could reorder this reservation, so the converged
        // leader atomic below is already Liverpool-exact. Do not collect/replay this shader: it
        // writes most of Buffer #2 before reaching DOC and a replay would duplicate those stores.
        ASSERT_MSG(control == DreamsCompat::QueueProducerPrimaryOrderedControl,
                   "Dreams 2bf DOC control changed: {:#x}", control);
        ASSERT_MSG(handle == 5, "Dreams 2bf GDS handle changed: expected 5, got {}", handle);
    }

    if (ctx.info.pgm_hash == DreamsCompat::SceneCompactShader &&
        DreamsCompat::UseSceneCompactExactOrderedCountReplay()) {
        // 393 launches sixteen Liverpool waves per workgroup. Each wave uses its own row of four
        // ordered counters, but every workgroup reuses those rows. Vulkan may run workgroups in a
        // different order, so collect all 64 wave/DOC payload streams, let the host scan them in
        // x-fast workgroup order, and replay once. The first three DOC results do not feed a later
        // DOC payload or control decision; Collect can return zero for them and terminate at the
        // fourth WAVE_DONE before either guest buffer side effect.
        ASSERT_MSG(Sirit::ValidId(ctx.dreams_ordered_phase),
                   "Dreams 393 DOC has no phase specialization constant");
        ASSERT_MSG(Sirit::ValidId(ctx.workgroup_index_id) &&
                       Sirit::ValidId(ctx.local_invocation_id),
                   "Dreams 393 DOC has no workgroup/local invocation metadata");
        ASSERT_MSG(handle == 5, "Dreams 393 GDS handle changed: expected 5, got {}", handle);

        u32 doc_index{};
        switch (control) {
        case 0x0004:
            doc_index = 0;
            break;
        case 0x0404:
            doc_index = 1;
            break;
        case 0x0804:
            doc_index = 2;
            break;
        case 0x0c0c:
            doc_index = 3;
            break;
        default:
            ASSERT_MSG(false, "Dreams 393 DOC control changed: {:#x}", control);
            break;
        }
        ASSERT_MSG(control == DreamsCompat::SceneCompactOrderedControls[doc_index],
                   "Dreams 393 DOC sequence/control mismatch: {} {:#x}", doc_index, control);

        const Id local_invocation = ctx.OpLoad(ctx.U32[3], ctx.local_invocation_id);
        const Id local_x = ctx.OpCompositeExtract(ctx.U32[1], local_invocation, 0);
        const Id wave_index = ctx.OpShiftRightLogical(
            ctx.U32[1], local_x, ctx.ConstU32(6U));
        const Id stream_index = ctx.OpIAdd(
            ctx.U32[1],
            ctx.OpIMul(ctx.U32[1], wave_index,
                       ctx.ConstU32(DreamsCompat::SceneCompactOrderedDocCount)),
            ctx.ConstU32(doc_index));
        const Id scratch_stream_base = ctx.OpIAdd(
            ctx.U32[1], ctx.ConstU32(DreamsCompat::SceneCompactOrderedScratchBaseDword),
            ctx.OpIMul(
                ctx.U32[1], stream_index,
                ctx.ConstU32(DreamsCompat::SceneCompactOrderedScratchStreamStrideDwords)));
        const Id scratch_entry = ctx.OpIAdd(
            ctx.U32[1], scratch_stream_base,
            ctx.OpIMul(ctx.U32[1], ctx.workgroup_index_id,
                       ctx.ConstU32(DreamsCompat::OrderedEntryDwords)));

        const bool is_wave_done = doc_index + 1 == DreamsCompat::SceneCompactOrderedDocCount;
        const Id phase = ctx.dreams_ordered_phase;
        const Id is_collect =
            ctx.OpIEqual(ctx.U1[1], phase, ctx.ConstU32(DreamsCompat::OrderedPhaseCollect));
        const Id collect_label = ctx.OpLabel();
        const Id non_collect_label = ctx.OpLabel();
        const Id phase_merge_label = ctx.OpLabel();
        ctx.OpSelectionMerge(phase_merge_label, spv::SelectionControlMask::MaskNone);
        ctx.OpBranchConditional(is_collect, collect_label, non_collect_label);

        ctx.AddLabel(collect_label);
        const Id collect_store_label = ctx.OpLabel();
        const Id collect_store_merge_label = ctx.OpLabel();
        ctx.OpSelectionMerge(collect_store_merge_label, spv::SelectionControlMask::MaskNone);
        ctx.OpBranchConditional(is_leader, collect_store_label, collect_store_merge_label);

        ctx.AddLabel(collect_store_label);
        ctx.OpStore(get_pointer(ctx.OpIAdd(
                        ctx.U32[1], scratch_entry,
                        ctx.ConstU32(DreamsCompat::SculptOrderedPayloadOffset))),
                    value);
        ctx.OpStore(get_pointer(ctx.OpIAdd(
                        ctx.U32[1], scratch_entry,
                        ctx.ConstU32(DreamsCompat::SculptOrderedTokenOffset))),
                    ordered_token);
        ctx.OpBranch(collect_store_merge_label);

        ctx.AddLabel(collect_store_merge_label);
        if (is_wave_done) {
            ctx.OpReturn();
        } else {
            ctx.OpBranch(phase_merge_label);
        }

        ctx.AddLabel(non_collect_label);
        const Id is_replay =
            ctx.OpIEqual(ctx.U1[1], phase, ctx.ConstU32(DreamsCompat::OrderedPhaseReplay));
        const Id replay_label = ctx.OpLabel();
        const Id native_label = ctx.OpLabel();
        const Id non_collect_merge_label = ctx.OpLabel();
        ctx.OpSelectionMerge(non_collect_merge_label, spv::SelectionControlMask::MaskNone);
        ctx.OpBranchConditional(is_replay, replay_label, native_label);

        ctx.AddLabel(replay_label);
        const Id replay_result = ctx.OpLoad(
            ctx.U32[1], get_pointer(ctx.OpIAdd(
                            ctx.U32[1], scratch_entry,
                            ctx.ConstU32(DreamsCompat::SculptOrderedPrefixOffset))));
        ctx.OpBranch(non_collect_merge_label);

        ctx.AddLabel(native_label);
        const Id native_delta =
            ctx.OpSelect(ctx.U32[1], is_leader, value, ctx.u32_zero_value);
        const Id native_previous = ctx.OpAtomicIAdd(ctx.U32[1], counter_ptr, device_scope,
                                                    acquire_release_semantics, native_delta);
        const Id native_result =
            ctx.OpGroupNonUniformBroadcastFirst(ctx.U32[1], subgroup_scope, native_previous);
        ctx.OpBranch(non_collect_merge_label);

        ctx.AddLabel(non_collect_merge_label);
        const Id non_collect_result =
            ctx.OpPhi(ctx.U32[1], replay_result, replay_label, native_result, native_label);
        ctx.OpBranch(phase_merge_label);

        ctx.AddLabel(phase_merge_label);
        if (is_wave_done) {
            return non_collect_result;
        }
        return ctx.OpPhi(ctx.U32[1], ctx.u32_zero_value, collect_store_merge_label,
                         non_collect_result, non_collect_merge_label);
    }

    if (ctx.info.pgm_hash == DreamsCompat::TraversalShader) {
        // b535 executes four independent ordered-count reservations before its first guest-visible
        // store. Collect their payloads in full WorkgroupIndex order, scan each counter stream, and
        // replay once. Returning zero from the first three collect DOCs is safe because none of
        // their results feeds a later DOC payload or control decision; WAVE_DONE is the fourth DOC.
        ASSERT_MSG(Sirit::ValidId(ctx.dreams_ordered_phase),
                   "Dreams b535 DOC has no phase specialization constant");
        ASSERT_MSG(Sirit::ValidId(ctx.workgroup_index_id) &&
                       Sirit::ValidId(ctx.num_workgroups_id),
                   "Dreams b535 DOC has no indirect workgroup metadata");

        u32 doc_index{};
        switch (control) {
        case 0x0804:
            doc_index = 0;
            break;
        case 0x1804:
            doc_index = 1;
            break;
        case 0x0c04:
            doc_index = 2;
            break;
        case 0x100c:
            doc_index = 3;
            break;
        default:
            ASSERT_MSG(false, "Dreams b535 DOC control changed: {:#x}", control);
            break;
        }
        ASSERT_MSG(control == DreamsCompat::TraversalOrderedControls[doc_index],
                   "Dreams b535 DOC sequence/control mismatch: {} {:#x}", doc_index, control);

        const Id num_workgroups = ctx.OpLoad(ctx.U32[3], ctx.num_workgroups_id);
        const Id num_workgroups_x = ctx.OpCompositeExtract(ctx.U32[1], num_workgroups, 0);
        const Id num_workgroups_y = ctx.OpCompositeExtract(ctx.U32[1], num_workgroups, 1);
        const Id num_workgroups_z = ctx.OpCompositeExtract(ctx.U32[1], num_workgroups, 2);
        const Id has_x_capacity =
            ctx.OpULessThanEqual(ctx.U1[1], num_workgroups_x,
                                 ctx.ConstU32(DreamsCompat::MaxTraversalWorkgroups));
        const Id has_one_y = ctx.OpIEqual(ctx.U1[1], num_workgroups_y, ctx.u32_one_value);
        const Id has_one_z = ctx.OpIEqual(ctx.U1[1], num_workgroups_z, ctx.u32_one_value);
        const Id has_exact_shape = ctx.OpLogicalAnd(
            ctx.U1[1], has_x_capacity, ctx.OpLogicalAnd(ctx.U1[1], has_one_y, has_one_z));

        const bool is_wave_done = doc_index + 1 == DreamsCompat::TraversalOrderedDocCount;
        const Id scratch_entry = ctx.OpIAdd(
            ctx.U32[1],
            ctx.ConstU32(DreamsCompat::TraversalOrderedScratchBaseDword(doc_index)),
            ctx.OpIMul(ctx.U32[1], ctx.workgroup_index_id,
                       ctx.ConstU32(DreamsCompat::OrderedEntryDwords)));

        const Id phase = ctx.dreams_ordered_phase;
        const Id is_collect =
            ctx.OpIEqual(ctx.U1[1], phase, ctx.ConstU32(DreamsCompat::OrderedPhaseCollect));
        const Id collect_label = ctx.OpLabel();
        const Id non_collect_label = ctx.OpLabel();
        const Id phase_merge_label = ctx.OpLabel();
        ctx.OpSelectionMerge(phase_merge_label, spv::SelectionControlMask::MaskNone);
        ctx.OpBranchConditional(is_collect, collect_label, non_collect_label);

        ctx.AddLabel(collect_label);
        const Id collect_store_label = ctx.OpLabel();
        const Id collect_store_merge_label = ctx.OpLabel();
        const Id should_collect_store = ctx.OpLogicalAnd(ctx.U1[1], has_exact_shape, is_leader);
        ctx.OpSelectionMerge(collect_store_merge_label, spv::SelectionControlMask::MaskNone);
        ctx.OpBranchConditional(should_collect_store, collect_store_label,
                                collect_store_merge_label);

        ctx.AddLabel(collect_store_label);
        ctx.OpStore(get_pointer(ctx.OpIAdd(
                        ctx.U32[1], scratch_entry,
                        ctx.ConstU32(DreamsCompat::SculptOrderedPayloadOffset))),
                    value);
        ctx.OpStore(get_pointer(ctx.OpIAdd(
                        ctx.U32[1], scratch_entry,
                        ctx.ConstU32(DreamsCompat::SculptOrderedTokenOffset))),
                    ordered_token);
        if (doc_index == 0) {
            // The dispatch dimensions are GPU-resident for this indirect shader. For the exact
            // supported shape y=z=1, x is the full group-count product; publish it once so the host
            // prefix pass can consume it without a queue idle/readback.
            const Id group_count_store_label = ctx.OpLabel();
            const Id group_count_store_merge_label = ctx.OpLabel();
            const Id is_first_workgroup =
                ctx.OpIEqual(ctx.U1[1], ctx.workgroup_index_id, ctx.u32_zero_value);
            ctx.OpSelectionMerge(group_count_store_merge_label,
                                 spv::SelectionControlMask::MaskNone);
            ctx.OpBranchConditional(is_first_workgroup, group_count_store_label,
                                    group_count_store_merge_label);

            ctx.AddLabel(group_count_store_label);
            ctx.OpStore(get_pointer(ctx.ConstU32(DreamsCompat::TraversalOrderedGroupCountDword)),
                        num_workgroups_x);
            ctx.OpBranch(group_count_store_merge_label);
            ctx.AddLabel(group_count_store_merge_label);
        }
        ctx.OpBranch(collect_store_merge_label);

        ctx.AddLabel(collect_store_merge_label);
        if (is_wave_done) {
            ctx.OpReturn();
        } else {
            ctx.OpBranch(phase_merge_label);
        }

        ctx.AddLabel(non_collect_label);
        const Id is_replay =
            ctx.OpIEqual(ctx.U1[1], phase, ctx.ConstU32(DreamsCompat::OrderedPhaseReplay));
        const Id replay_label = ctx.OpLabel();
        const Id native_label = ctx.OpLabel();
        const Id non_collect_merge_label = ctx.OpLabel();
        // Unsupported indirect shapes never touched scratch in Collect. Replay those shapes
        // through the native DOC exactly once, while the host observes a cleared zero group count
        // and therefore performs no prefix scan.
        const Id use_exact_replay = ctx.OpLogicalAnd(ctx.U1[1], is_replay, has_exact_shape);
        ctx.OpSelectionMerge(non_collect_merge_label, spv::SelectionControlMask::MaskNone);
        ctx.OpBranchConditional(use_exact_replay, replay_label, native_label);

        ctx.AddLabel(replay_label);
        const Id replay_result = ctx.OpLoad(
            ctx.U32[1], get_pointer(ctx.OpIAdd(
                            ctx.U32[1], scratch_entry,
                            ctx.ConstU32(DreamsCompat::SculptOrderedPrefixOffset))));
        ctx.OpBranch(non_collect_merge_label);

        ctx.AddLabel(native_label);
        const Id native_counter_ptr = get_pointer(
            ctx.ConstU32(DreamsCompat::TraversalOrderedCounterIndices[doc_index]));
        const Id native_delta =
            ctx.OpSelect(ctx.U32[1], is_leader, value, ctx.u32_zero_value);
        const Id native_previous = ctx.OpAtomicIAdd(ctx.U32[1], native_counter_ptr, device_scope,
                                                    acquire_release_semantics, native_delta);
        const Id native_result =
            ctx.OpGroupNonUniformBroadcastFirst(ctx.U32[1], subgroup_scope, native_previous);
        ctx.OpBranch(non_collect_merge_label);

        ctx.AddLabel(non_collect_merge_label);
        const Id non_collect_result =
            ctx.OpPhi(ctx.U32[1], replay_result, replay_label, native_result, native_label);
        ctx.OpBranch(phase_merge_label);

        ctx.AddLabel(phase_merge_label);
        if (is_wave_done) {
            return non_collect_result;
        }
        return ctx.OpPhi(ctx.U32[1], ctx.u32_zero_value, collect_store_merge_label,
                         non_collect_result, non_collect_merge_label);
    }

    if (ctx.info.pgm_hash == DreamsCompat::QueueProducerShaderAlt) {
        // 692 makes two queue reservations before its first guest-visible store. Vulkan device
        // atomics preserve the totals but assign their return values in host arrival order.
        // Collect both wave payloads, scan the two guest counters in x-fast creation order, then
        // replay the output stores once with the exact Liverpool return values.
        ASSERT_MSG(Sirit::ValidId(ctx.dreams_ordered_phase),
                   "Dreams 692 DOC has no phase specialization constant");
        ASSERT_MSG(Sirit::ValidId(ctx.workgroup_index_id) &&
                       Sirit::ValidId(ctx.num_workgroups_id),
                   "Dreams 692 DOC has no indirect workgroup metadata");

        u32 doc_index{};
        switch (control) {
        case 0x0c04:
            doc_index = 0;
            break;
        case 0x140c:
            doc_index = 1;
            break;
        default:
            ASSERT_MSG(false, "Dreams 692 DOC control changed: {:#x}", control);
            break;
        }
        ASSERT_MSG(control == DreamsCompat::QueueProducerAltOrderedControls[doc_index],
                   "Dreams 692 DOC sequence/control mismatch: {} {:#x}", doc_index, control);
        ASSERT_MSG(handle == 7, "Dreams 692 GDS handle changed: expected 7, got {}", handle);

        const Id num_workgroups = ctx.OpLoad(ctx.U32[3], ctx.num_workgroups_id);
        const Id num_workgroups_x = ctx.OpCompositeExtract(ctx.U32[1], num_workgroups, 0);
        const Id num_workgroups_y = ctx.OpCompositeExtract(ctx.U32[1], num_workgroups, 1);
        const Id num_workgroups_z = ctx.OpCompositeExtract(ctx.U32[1], num_workgroups, 2);
        const Id has_x_capacity =
            ctx.OpULessThanEqual(ctx.U1[1], num_workgroups_x,
                                 ctx.ConstU32(DreamsCompat::MaxTraversalWorkgroups));
        const Id has_one_y = ctx.OpIEqual(ctx.U1[1], num_workgroups_y, ctx.u32_one_value);
        const Id has_one_z = ctx.OpIEqual(ctx.U1[1], num_workgroups_z, ctx.u32_one_value);
        const Id has_exact_shape = ctx.OpLogicalAnd(
            ctx.U1[1], has_x_capacity, ctx.OpLogicalAnd(ctx.U1[1], has_one_y, has_one_z));
        const Id scratch_entry = ctx.OpIAdd(
            ctx.U32[1],
            ctx.ConstU32(DreamsCompat::QueueProducerAltOrderedScratchBaseDword(doc_index)),
            ctx.OpIMul(ctx.U32[1], ctx.workgroup_index_id,
                       ctx.ConstU32(DreamsCompat::OrderedEntryDwords)));

        const bool is_wave_done =
            doc_index + 1 == DreamsCompat::QueueProducerAltOrderedDocCount;
        const Id phase = ctx.dreams_ordered_phase;
        const Id is_collect =
            ctx.OpIEqual(ctx.U1[1], phase, ctx.ConstU32(DreamsCompat::OrderedPhaseCollect));
        const Id collect_label = ctx.OpLabel();
        const Id non_collect_label = ctx.OpLabel();
        const Id phase_merge_label = ctx.OpLabel();
        ctx.OpSelectionMerge(phase_merge_label, spv::SelectionControlMask::MaskNone);
        ctx.OpBranchConditional(is_collect, collect_label, non_collect_label);

        ctx.AddLabel(collect_label);
        const Id collect_store_label = ctx.OpLabel();
        const Id collect_store_merge_label = ctx.OpLabel();
        const Id should_collect_store = ctx.OpLogicalAnd(ctx.U1[1], has_exact_shape, is_leader);
        ctx.OpSelectionMerge(collect_store_merge_label, spv::SelectionControlMask::MaskNone);
        ctx.OpBranchConditional(should_collect_store, collect_store_label,
                                collect_store_merge_label);

        ctx.AddLabel(collect_store_label);
        ctx.OpStore(get_pointer(ctx.OpIAdd(
                        ctx.U32[1], scratch_entry,
                        ctx.ConstU32(DreamsCompat::SculptOrderedPayloadOffset))),
                    value);
        ctx.OpStore(get_pointer(ctx.OpIAdd(
                        ctx.U32[1], scratch_entry,
                        ctx.ConstU32(DreamsCompat::SculptOrderedTokenOffset))),
                    ordered_token);
        if (doc_index == 0) {
            const Id group_count_store_label = ctx.OpLabel();
            const Id group_count_store_merge_label = ctx.OpLabel();
            const Id is_first_workgroup =
                ctx.OpIEqual(ctx.U1[1], ctx.workgroup_index_id, ctx.u32_zero_value);
            ctx.OpSelectionMerge(group_count_store_merge_label,
                                 spv::SelectionControlMask::MaskNone);
            ctx.OpBranchConditional(is_first_workgroup, group_count_store_label,
                                    group_count_store_merge_label);

            ctx.AddLabel(group_count_store_label);
            ctx.OpStore(
                get_pointer(ctx.ConstU32(DreamsCompat::QueueProducerAltOrderedGroupCountDword)),
                num_workgroups_x);
            ctx.OpBranch(group_count_store_merge_label);
            ctx.AddLabel(group_count_store_merge_label);
        }
        ctx.OpBranch(collect_store_merge_label);

        ctx.AddLabel(collect_store_merge_label);
        if (is_wave_done) {
            ctx.OpReturn();
        } else {
            ctx.OpBranch(phase_merge_label);
        }

        ctx.AddLabel(non_collect_label);
        const Id is_replay =
            ctx.OpIEqual(ctx.U1[1], phase, ctx.ConstU32(DreamsCompat::OrderedPhaseReplay));
        const Id replay_label = ctx.OpLabel();
        const Id native_label = ctx.OpLabel();
        const Id non_collect_merge_label = ctx.OpLabel();
        const Id use_exact_replay = ctx.OpLogicalAnd(ctx.U1[1], is_replay, has_exact_shape);
        ctx.OpSelectionMerge(non_collect_merge_label, spv::SelectionControlMask::MaskNone);
        ctx.OpBranchConditional(use_exact_replay, replay_label, native_label);

        ctx.AddLabel(replay_label);
        const Id replay_result = ctx.OpLoad(
            ctx.U32[1], get_pointer(ctx.OpIAdd(
                            ctx.U32[1], scratch_entry,
                            ctx.ConstU32(DreamsCompat::SculptOrderedPrefixOffset))));
        ctx.OpBranch(non_collect_merge_label);

        ctx.AddLabel(native_label);
        const Id native_counter_ptr = get_pointer(
            ctx.ConstU32(DreamsCompat::QueueProducerAltOrderedCounterIndices[doc_index]));
        const Id native_delta =
            ctx.OpSelect(ctx.U32[1], is_leader, value, ctx.u32_zero_value);
        const Id native_previous = ctx.OpAtomicIAdd(ctx.U32[1], native_counter_ptr, device_scope,
                                                    acquire_release_semantics, native_delta);
        const Id native_result =
            ctx.OpGroupNonUniformBroadcastFirst(ctx.U32[1], subgroup_scope, native_previous);
        ctx.OpBranch(non_collect_merge_label);

        ctx.AddLabel(non_collect_merge_label);
        const Id non_collect_result =
            ctx.OpPhi(ctx.U32[1], replay_result, replay_label, native_result, native_label);
        ctx.OpBranch(phase_merge_label);

        ctx.AddLabel(phase_merge_label);
        if (is_wave_done) {
            return non_collect_result;
        }
        return ctx.OpPhi(ctx.U32[1], ctx.u32_zero_value, collect_store_merge_label,
                         non_collect_result, non_collect_merge_label);
    }

    const bool is_dreams_b1_seed_writer =
        ctx.info.pgm_hash == DreamsCompat::B1SeedWriterShader;
    const bool is_dreams_visibility_candidate =
        ctx.info.pgm_hash == DreamsCompat::VisibilityCandidateCompactShader;
    const bool is_dreams_gather_input =
        ctx.info.pgm_hash == DreamsCompat::GatherInputCompactShader;
    if (is_dreams_b1_seed_writer || is_dreams_visibility_candidate || is_dreams_gather_input) {
        // 4eb, d8 and 95c each have one DOC, one guest wave per workgroup, and no guest-visible
        // store before DOC. Collect records the wave payload and stops; a host scan assigns
        // prefixes in WorkgroupIndex order; replay then executes the output stores exactly once.
        const u32 expected_control =
            is_dreams_b1_seed_writer
                ? DreamsCompat::B1SeedWriterOrderedControl
                : (is_dreams_gather_input ? DreamsCompat::GatherInputOrderedControl
                                          : DreamsCompat::VisibilityCandidateOrderedControl);
        ASSERT_MSG(packed_control == expected_control,
                   "Dreams single-DOC control changed for {:#x}: expected {:#x}, got {:#x}",
                   ctx.info.pgm_hash, expected_control, packed_control);
        ASSERT_MSG(Sirit::ValidId(ctx.dreams_ordered_phase),
                   "Dreams radix DOC has no phase specialization constant");
        ASSERT_MSG(Sirit::ValidId(ctx.workgroup_index_id) &&
                       Sirit::ValidId(ctx.num_workgroups_id),
                   "Dreams radix DOC has no workgroup metadata");

        const Id num_workgroups = ctx.OpLoad(ctx.U32[3], ctx.num_workgroups_id);
        const Id num_workgroups_x = ctx.OpCompositeExtract(ctx.U32[1], num_workgroups, 0);
        const Id num_workgroups_y = ctx.OpCompositeExtract(ctx.U32[1], num_workgroups, 1);
        const Id num_workgroups_z = ctx.OpCompositeExtract(ctx.U32[1], num_workgroups, 2);
        const Id has_x_capacity =
            ctx.OpULessThanEqual(ctx.U1[1], num_workgroups_x,
                                 ctx.ConstU32(DreamsCompat::MaxTraversalWorkgroups));
        const Id has_one_y = ctx.OpIEqual(ctx.U1[1], num_workgroups_y, ctx.u32_one_value);
        const Id has_one_z = ctx.OpIEqual(ctx.U1[1], num_workgroups_z, ctx.u32_one_value);
        const Id has_exact_shape = ctx.OpLogicalAnd(
            ctx.U1[1], has_x_capacity, ctx.OpLogicalAnd(ctx.U1[1], has_one_y, has_one_z));
        const u32 scratch_base = is_dreams_b1_seed_writer
                                     ? DreamsCompat::B1SeedWriterOrderedScratchBaseDword
                                     : DreamsCompat::VisibilityCandidateOrderedScratchBaseDword;
        const Id scratch_entry = ctx.OpIAdd(
            ctx.U32[1], ctx.ConstU32(scratch_base),
            ctx.OpIMul(ctx.U32[1], ctx.workgroup_index_id,
                       ctx.ConstU32(DreamsCompat::OrderedEntryDwords)));

        const Id phase = ctx.dreams_ordered_phase;
        const Id is_collect =
            ctx.OpIEqual(ctx.U1[1], phase, ctx.ConstU32(DreamsCompat::OrderedPhaseCollect));
        const Id collect_label = ctx.OpLabel();
        const Id non_collect_label = ctx.OpLabel();
        const Id phase_merge_label = ctx.OpLabel();
        ctx.OpSelectionMerge(phase_merge_label, spv::SelectionControlMask::MaskNone);
        ctx.OpBranchConditional(is_collect, collect_label, non_collect_label);

        ctx.AddLabel(collect_label);
        const Id collect_store_label = ctx.OpLabel();
        const Id collect_store_merge_label = ctx.OpLabel();
        const Id should_collect_store = ctx.OpLogicalAnd(ctx.U1[1], has_exact_shape, is_leader);
        ctx.OpSelectionMerge(collect_store_merge_label, spv::SelectionControlMask::MaskNone);
        ctx.OpBranchConditional(should_collect_store, collect_store_label,
                                collect_store_merge_label);

        ctx.AddLabel(collect_store_label);
        ctx.OpStore(get_pointer(ctx.OpIAdd(
                        ctx.U32[1], scratch_entry,
                        ctx.ConstU32(DreamsCompat::SculptOrderedPayloadOffset))),
                    value);
        ctx.OpStore(get_pointer(ctx.OpIAdd(
                        ctx.U32[1], scratch_entry,
                        ctx.ConstU32(DreamsCompat::SculptOrderedTokenOffset))),
                    ordered_token);
        if (is_dreams_visibility_candidate) {
            // d8 is dispatched indirectly, so publish its GPU-resident x dimension for the host
            // prefix helper. 4eb and 95c are direct, so the host knows their exact group count.
            const Id group_count_store_label = ctx.OpLabel();
            const Id group_count_store_merge_label = ctx.OpLabel();
            const Id is_first_workgroup =
                ctx.OpIEqual(ctx.U1[1], ctx.workgroup_index_id, ctx.u32_zero_value);
            ctx.OpSelectionMerge(group_count_store_merge_label,
                                 spv::SelectionControlMask::MaskNone);
            ctx.OpBranchConditional(is_first_workgroup, group_count_store_label,
                                    group_count_store_merge_label);

            ctx.AddLabel(group_count_store_label);
            ctx.OpStore(
                get_pointer(ctx.ConstU32(DreamsCompat::VisibilityCandidateOrderedGroupCountDword)),
                num_workgroups_x);
            ctx.OpBranch(group_count_store_merge_label);
            ctx.AddLabel(group_count_store_merge_label);
        }
        ctx.OpBranch(collect_store_merge_label);

        ctx.AddLabel(collect_store_merge_label);
        ctx.OpReturn();

        ctx.AddLabel(non_collect_label);
        const Id is_replay =
            ctx.OpIEqual(ctx.U1[1], phase, ctx.ConstU32(DreamsCompat::OrderedPhaseReplay));
        const Id replay_label = ctx.OpLabel();
        const Id native_label = ctx.OpLabel();
        const Id non_collect_merge_label = ctx.OpLabel();
        const Id use_exact_replay = ctx.OpLogicalAnd(ctx.U1[1], is_replay, has_exact_shape);
        ctx.OpSelectionMerge(non_collect_merge_label, spv::SelectionControlMask::MaskNone);
        ctx.OpBranchConditional(use_exact_replay, replay_label, native_label);

        ctx.AddLabel(replay_label);
        const Id replay_result = ctx.OpLoad(
            ctx.U32[1], get_pointer(ctx.OpIAdd(
                            ctx.U32[1], scratch_entry,
                            ctx.ConstU32(DreamsCompat::SculptOrderedPrefixOffset))));
        ctx.OpBranch(non_collect_merge_label);

        ctx.AddLabel(native_label);
        const Id native_delta =
            ctx.OpSelect(ctx.U32[1], is_leader, value, ctx.u32_zero_value);
        const Id native_previous = ctx.OpAtomicIAdd(ctx.U32[1], counter_ptr, device_scope,
                                                    acquire_release_semantics, native_delta);
        const Id native_result =
            ctx.OpGroupNonUniformBroadcastFirst(ctx.U32[1], subgroup_scope, native_previous);
        ctx.OpBranch(non_collect_merge_label);

        ctx.AddLabel(non_collect_merge_label);
        const Id result =
            ctx.OpPhi(ctx.U32[1], replay_result, replay_label, native_result, native_label);
        ctx.OpBranch(phase_merge_label);

        ctx.AddLabel(phase_merge_label);
        return result;
    }

    if (DreamsCompat::UsesExactOrderedCountReplay(ctx.info.pgm_hash) &&
        (ctx.info.pgm_hash == DreamsCompat::VisibilityListCompactShader ||
         ctx.info.pgm_hash == DreamsCompat::VisibilityListCompactShaderAlt)) {
        // 7aa/016 each make two independent category reservations before either guest-visible
        // output store. A native device atomic preserves the totals but assigns whole wave chunks
        // in host arrival order, undoing d8's stable radix order. Collect both payloads in x-fast
        // WorkgroupIndex order, scan the two guest counters, then replay both output stores once.
        ASSERT_MSG(Sirit::ValidId(ctx.dreams_ordered_phase),
                   "Dreams 7aa/016 DOC has no phase specialization constant");
        ASSERT_MSG(Sirit::ValidId(ctx.workgroup_index_id) &&
                       Sirit::ValidId(ctx.num_workgroups_id),
                   "Dreams 7aa/016 DOC has no workgroup creation index");

        u32 doc_index{};
        switch (control) {
        case 0x0004:
            doc_index = 0;
            break;
        case 0x040c:
            doc_index = 1;
            break;
        default:
            ASSERT_MSG(false, "Dreams 7aa/016 DOC control changed: {:#x}", control);
            break;
        }
        ASSERT_MSG(control == DreamsCompat::VisibilityListOrderedControls[doc_index],
                   "Dreams 7aa/016 DOC sequence/control mismatch: {} {:#x}", doc_index, control);
        const u32 expected_handle =
            ctx.info.pgm_hash == DreamsCompat::VisibilityListCompactShader ? 3 : 2;
        ASSERT_MSG(handle == expected_handle,
                   "Dreams 7aa/016 GDS handle changed: expected {}, got {}", expected_handle,
                   handle);

        const Id num_workgroups = ctx.OpLoad(ctx.U32[3], ctx.num_workgroups_id);
        const Id num_workgroups_x = ctx.OpCompositeExtract(ctx.U32[1], num_workgroups, 0);
        const Id num_workgroups_y = ctx.OpCompositeExtract(ctx.U32[1], num_workgroups, 1);
        const Id num_workgroups_z = ctx.OpCompositeExtract(ctx.U32[1], num_workgroups, 2);
        const Id has_x_capacity =
            ctx.OpULessThanEqual(ctx.U1[1], num_workgroups_x,
                                 ctx.ConstU32(DreamsCompat::MaxTraversalWorkgroups));
        const Id has_one_y = ctx.OpIEqual(ctx.U1[1], num_workgroups_y, ctx.u32_one_value);
        const Id has_one_z = ctx.OpIEqual(ctx.U1[1], num_workgroups_z, ctx.u32_one_value);
        const Id has_exact_shape = ctx.OpLogicalAnd(
            ctx.U1[1], has_x_capacity, ctx.OpLogicalAnd(ctx.U1[1], has_one_y, has_one_z));
        const Id scratch_entry = ctx.OpIAdd(
            ctx.U32[1],
            ctx.ConstU32(DreamsCompat::VisibilityListOrderedScratchBaseDword(doc_index)),
            ctx.OpIMul(ctx.U32[1], ctx.workgroup_index_id,
                       ctx.ConstU32(DreamsCompat::OrderedEntryDwords)));

        const bool is_wave_done = doc_index + 1 == DreamsCompat::VisibilityListOrderedDocCount;
        const Id phase = ctx.dreams_ordered_phase;
        const Id is_collect =
            ctx.OpIEqual(ctx.U1[1], phase, ctx.ConstU32(DreamsCompat::OrderedPhaseCollect));
        const Id collect_label = ctx.OpLabel();
        const Id non_collect_label = ctx.OpLabel();
        const Id phase_merge_label = ctx.OpLabel();
        ctx.OpSelectionMerge(phase_merge_label, spv::SelectionControlMask::MaskNone);
        ctx.OpBranchConditional(is_collect, collect_label, non_collect_label);

        ctx.AddLabel(collect_label);
        const Id collect_store_label = ctx.OpLabel();
        const Id collect_store_merge_label = ctx.OpLabel();
        const Id should_collect_store = ctx.OpLogicalAnd(ctx.U1[1], has_exact_shape, is_leader);
        ctx.OpSelectionMerge(collect_store_merge_label, spv::SelectionControlMask::MaskNone);
        ctx.OpBranchConditional(should_collect_store, collect_store_label,
                                collect_store_merge_label);

        ctx.AddLabel(collect_store_label);
        ctx.OpStore(get_pointer(ctx.OpIAdd(
                        ctx.U32[1], scratch_entry,
                        ctx.ConstU32(DreamsCompat::SculptOrderedPayloadOffset))),
                    value);
        ctx.OpStore(get_pointer(ctx.OpIAdd(
                        ctx.U32[1], scratch_entry,
                        ctx.ConstU32(DreamsCompat::SculptOrderedTokenOffset))),
                    ordered_token);
        if (doc_index == 0) {
            // Workgroup zero publishes the GPU-resident indirect x dimension. The supported
            // y=z=1 shape makes x the complete guest creation-order range for both streams.
            const Id group_count_store_label = ctx.OpLabel();
            const Id group_count_store_merge_label = ctx.OpLabel();
            const Id is_first_workgroup =
                ctx.OpIEqual(ctx.U1[1], ctx.workgroup_index_id, ctx.u32_zero_value);
            ctx.OpSelectionMerge(group_count_store_merge_label,
                                 spv::SelectionControlMask::MaskNone);
            ctx.OpBranchConditional(is_first_workgroup, group_count_store_label,
                                    group_count_store_merge_label);

            ctx.AddLabel(group_count_store_label);
            ctx.OpStore(
                get_pointer(ctx.ConstU32(DreamsCompat::VisibilityListOrderedGroupCountDword)),
                num_workgroups_x);
            ctx.OpBranch(group_count_store_merge_label);
            ctx.AddLabel(group_count_store_merge_label);
        }
        ctx.OpBranch(collect_store_merge_label);

        ctx.AddLabel(collect_store_merge_label);
        if (is_wave_done) {
            ctx.OpReturn();
        } else {
            ctx.OpBranch(phase_merge_label);
        }

        ctx.AddLabel(non_collect_label);
        const Id is_replay =
            ctx.OpIEqual(ctx.U1[1], phase, ctx.ConstU32(DreamsCompat::OrderedPhaseReplay));
        const Id replay_label = ctx.OpLabel();
        const Id native_label = ctx.OpLabel();
        const Id non_collect_merge_label = ctx.OpLabel();
        // Unsupported indirect shapes intentionally execute a no-op collect and then run the
        // original atomic/store path exactly once during replay.
        const Id use_exact_replay = ctx.OpLogicalAnd(ctx.U1[1], is_replay, has_exact_shape);
        ctx.OpSelectionMerge(non_collect_merge_label, spv::SelectionControlMask::MaskNone);
        ctx.OpBranchConditional(use_exact_replay, replay_label, native_label);

        ctx.AddLabel(replay_label);
        const Id replay_result = ctx.OpLoad(
            ctx.U32[1], get_pointer(ctx.OpIAdd(
                            ctx.U32[1], scratch_entry,
                            ctx.ConstU32(DreamsCompat::SculptOrderedPrefixOffset))));
        ctx.OpBranch(non_collect_merge_label);

        ctx.AddLabel(native_label);
        const Id native_delta =
            ctx.OpSelect(ctx.U32[1], is_leader, value, ctx.u32_zero_value);
        const Id native_previous = ctx.OpAtomicIAdd(ctx.U32[1], counter_ptr, device_scope,
                                                    acquire_release_semantics, native_delta);
        const Id native_result =
            ctx.OpGroupNonUniformBroadcastFirst(ctx.U32[1], subgroup_scope, native_previous);
        ctx.OpBranch(non_collect_merge_label);

        ctx.AddLabel(non_collect_merge_label);
        const Id non_collect_result =
            ctx.OpPhi(ctx.U32[1], replay_result, replay_label, native_result, native_label);
        ctx.OpBranch(phase_merge_label);

        ctx.AddLabel(phase_merge_label);
        if (is_wave_done) {
            return non_collect_result;
        }
        return ctx.OpPhi(ctx.U32[1], ctx.u32_zero_value, collect_store_merge_label,
                         non_collect_result, non_collect_merge_label);
    }

    if (ctx.info.pgm_hash == DreamsCompat::IndirectArgsShader) {
        // 902 has two DOCs and one guest wave per workgroup. Vulkan may schedule those waves in
        // any order, so a device atomic cannot reproduce Liverpool's wave-creation-order return
        // values. Collect both payloads without executing guest stores, scan them in the guest's
        // x-fast workgroup order, then replay the shader with the resulting prefixes. The first
        // DOC result does not feed the second DOC payload or control flow; collect may therefore
        // use zero for it and terminate at the second DOC before every guest-visible store.
        ASSERT_MSG(control == 4 || control == 12,
                   "Dreams 902 DOC control changed: {:#x}", control);
        ASSERT_MSG(Sirit::ValidId(ctx.dreams_ordered_phase),
                   "Dreams 902 DOC has no phase specialization constant");
        ASSERT_MSG(Sirit::ValidId(ctx.workgroup_id) &&
                       Sirit::ValidId(ctx.workgroup_index_id),
                   "Dreams 902 DOC has no workgroup creation index");

        const bool is_row_prefix = control == 4;
        const Id workgroup = ctx.OpLoad(ctx.U32[3], ctx.workgroup_id);
        const Id workgroup_x = ctx.OpCompositeExtract(ctx.U32[1], workgroup, 0);
        const Id workgroup_y = ctx.OpCompositeExtract(ctx.U32[1], workgroup, 1);
        const Id scratch_entry = [&] {
            if (is_row_prefix) {
                const Id row_base = ctx.OpIAdd(
                    ctx.U32[1],
                    ctx.ConstU32(DreamsCompat::IndirectArgsOrderedRowScratchBaseDword(0)),
                    ctx.OpIMul(ctx.U32[1], workgroup_y,
                               ctx.ConstU32(DreamsCompat::OrderedCounterStrideDwords)));
                return ctx.OpIAdd(
                    ctx.U32[1], row_base,
                    ctx.OpIMul(ctx.U32[1], workgroup_x,
                               ctx.ConstU32(DreamsCompat::OrderedEntryDwords)));
            }
            return ctx.OpIAdd(
                ctx.U32[1],
                ctx.ConstU32(DreamsCompat::IndirectArgsOrderedGlobalScratchBaseDword),
                ctx.OpIMul(ctx.U32[1], ctx.workgroup_index_id,
                           ctx.ConstU32(DreamsCompat::OrderedEntryDwords)));
        }();

        const Id phase = ctx.dreams_ordered_phase;
        const Id is_collect =
            ctx.OpIEqual(ctx.U1[1], phase, ctx.ConstU32(DreamsCompat::OrderedPhaseCollect));
        const Id collect_label = ctx.OpLabel();
        const Id non_collect_label = ctx.OpLabel();
        const Id phase_merge_label = ctx.OpLabel();
        ctx.OpSelectionMerge(phase_merge_label, spv::SelectionControlMask::MaskNone);
        ctx.OpBranchConditional(is_collect, collect_label, non_collect_label);

        ctx.AddLabel(collect_label);
        const Id collect_store_label = ctx.OpLabel();
        const Id collect_store_merge_label = ctx.OpLabel();
        ctx.OpSelectionMerge(collect_store_merge_label, spv::SelectionControlMask::MaskNone);
        ctx.OpBranchConditional(is_leader, collect_store_label, collect_store_merge_label);

        ctx.AddLabel(collect_store_label);
        ctx.OpStore(get_pointer(ctx.OpIAdd(
                        ctx.U32[1], scratch_entry,
                        ctx.ConstU32(DreamsCompat::SculptOrderedPayloadOffset))),
                    value);
        ctx.OpStore(get_pointer(ctx.OpIAdd(
                        ctx.U32[1], scratch_entry,
                        ctx.ConstU32(DreamsCompat::SculptOrderedTokenOffset))),
                    ordered_token);
        ctx.OpBranch(collect_store_merge_label);

        ctx.AddLabel(collect_store_merge_label);
        if (is_row_prefix) {
            ctx.OpBranch(phase_merge_label);
        } else {
            ctx.OpReturn();
        }

        ctx.AddLabel(non_collect_label);
        const Id is_replay =
            ctx.OpIEqual(ctx.U1[1], phase, ctx.ConstU32(DreamsCompat::OrderedPhaseReplay));
        const Id replay_label = ctx.OpLabel();
        const Id native_label = ctx.OpLabel();
        const Id non_collect_merge_label = ctx.OpLabel();
        ctx.OpSelectionMerge(non_collect_merge_label, spv::SelectionControlMask::MaskNone);
        ctx.OpBranchConditional(is_replay, replay_label, native_label);

        ctx.AddLabel(replay_label);
        const Id replay_result = ctx.OpLoad(
            ctx.U32[1], get_pointer(ctx.OpIAdd(
                            ctx.U32[1], scratch_entry,
                            ctx.ConstU32(DreamsCompat::SculptOrderedPrefixOffset))));
        ctx.OpBranch(non_collect_merge_label);

        ctx.AddLabel(native_label);
        const Id native_delta =
            ctx.OpSelect(ctx.U32[1], is_leader, value, ctx.u32_zero_value);
        const Id native_previous = ctx.OpAtomicIAdd(ctx.U32[1], counter_ptr, device_scope,
                                                    acquire_release_semantics, native_delta);
        const Id native_result =
            ctx.OpGroupNonUniformBroadcastFirst(ctx.U32[1], subgroup_scope, native_previous);
        ctx.OpBranch(non_collect_merge_label);

        ctx.AddLabel(non_collect_merge_label);
        const Id non_collect_result =
            ctx.OpPhi(ctx.U32[1], replay_result, replay_label, native_result, native_label);
        ctx.OpBranch(phase_merge_label);

        ctx.AddLabel(phase_merge_label);
        if (is_row_prefix) {
            return ctx.OpPhi(ctx.U32[1], ctx.u32_zero_value, collect_store_merge_label,
                             non_collect_result, non_collect_merge_label);
        }
        return non_collect_result;
    }

    if (ctx.info.pgm_hash == DreamsCompat::SculptSurfaceCompactShader ||
        ctx.info.pgm_hash == DreamsCompat::SculptSurfaceFinalizeShader) {
        // f030 and 5ac each have one 64-lane wave per 4x4x4 workgroup and one add+WAVE_DONE DOC.
        // Their logical terminal workgroup publishes prefix+payload as the compact record count,
        // so an unordered device atomic can publish a partial count when that workgroup runs
        // early. Collect each payload by flattened x-fast WorkgroupIndex, scan from the existing
        // guest GDS base, and replay with the exact Liverpool prefix. Neither shader has a guest
        // side effect before its DOC, so Collect can terminate here without a state checkpoint.
        const u32 expected_control =
            ctx.info.pgm_hash == DreamsCompat::SculptSurfaceCompactShader
                ? DreamsCompat::SculptSurfaceOrderedControl
                : DreamsCompat::SculptSurfaceFinalizeOrderedControl;
        ASSERT_MSG(packed_control == expected_control,
                   "Dreams sculpt-surface DOC control changed for {:#x}: expected {:#x}, got {:#x}",
                   ctx.info.pgm_hash, expected_control, packed_control);
        ASSERT_MSG(Sirit::ValidId(ctx.dreams_ordered_phase),
                   "Dreams sculpt-surface DOC has no phase specialization constant");
        ASSERT_MSG(Sirit::ValidId(ctx.workgroup_index_id),
                   "Dreams sculpt-surface DOC has no workgroup creation index");

        const Id scratch_entry = ctx.OpIAdd(
            ctx.U32[1], ctx.ConstU32(DreamsCompat::SculptSurfaceOrderedScratchBaseDword),
            ctx.OpIMul(ctx.U32[1], ctx.workgroup_index_id,
                       ctx.ConstU32(DreamsCompat::OrderedEntryDwords)));
        const Id phase = ctx.dreams_ordered_phase;
        const Id is_collect =
            ctx.OpIEqual(ctx.U1[1], phase, ctx.ConstU32(DreamsCompat::OrderedPhaseCollect));
        const Id collect_label = ctx.OpLabel();
        const Id non_collect_label = ctx.OpLabel();
        const Id phase_merge_label = ctx.OpLabel();
        ctx.OpSelectionMerge(phase_merge_label, spv::SelectionControlMask::MaskNone);
        ctx.OpBranchConditional(is_collect, collect_label, non_collect_label);

        ctx.AddLabel(collect_label);
        const Id collect_store_label = ctx.OpLabel();
        const Id collect_store_merge_label = ctx.OpLabel();
        ctx.OpSelectionMerge(collect_store_merge_label, spv::SelectionControlMask::MaskNone);
        ctx.OpBranchConditional(is_leader, collect_store_label, collect_store_merge_label);

        ctx.AddLabel(collect_store_label);
        ctx.OpStore(get_pointer(ctx.OpIAdd(
                        ctx.U32[1], scratch_entry,
                        ctx.ConstU32(DreamsCompat::SculptOrderedPayloadOffset))),
                    value);
        ctx.OpStore(get_pointer(ctx.OpIAdd(
                        ctx.U32[1], scratch_entry,
                        ctx.ConstU32(DreamsCompat::SculptOrderedTokenOffset))),
                    ordered_token);
        ctx.OpBranch(collect_store_merge_label);

        ctx.AddLabel(collect_store_merge_label);
        ctx.OpReturn();

        ctx.AddLabel(non_collect_label);
        const Id is_replay =
            ctx.OpIEqual(ctx.U1[1], phase, ctx.ConstU32(DreamsCompat::OrderedPhaseReplay));
        const Id replay_label = ctx.OpLabel();
        const Id native_label = ctx.OpLabel();
        const Id non_collect_merge_label = ctx.OpLabel();
        ctx.OpSelectionMerge(non_collect_merge_label, spv::SelectionControlMask::MaskNone);
        ctx.OpBranchConditional(is_replay, replay_label, native_label);

        ctx.AddLabel(replay_label);
        const Id replay_result = ctx.OpLoad(
            ctx.U32[1], get_pointer(ctx.OpIAdd(
                            ctx.U32[1], scratch_entry,
                            ctx.ConstU32(DreamsCompat::SculptOrderedPrefixOffset))));
        ctx.OpBranch(non_collect_merge_label);

        ctx.AddLabel(native_label);
        // Keep all active lanes converged through the fallback atomic and broadcast. Only the
        // elected first lane contributes the wave payload.
        const Id native_delta =
            ctx.OpSelect(ctx.U32[1], is_leader, value, ctx.u32_zero_value);
        const Id native_previous = ctx.OpAtomicIAdd(ctx.U32[1], counter_ptr, device_scope,
                                                    acquire_release_semantics, native_delta);
        const Id native_result =
            ctx.OpGroupNonUniformBroadcastFirst(ctx.U32[1], subgroup_scope, native_previous);
        ctx.OpBranch(non_collect_merge_label);

        ctx.AddLabel(non_collect_merge_label);
        const Id result =
            ctx.OpPhi(ctx.U32[1], replay_result, replay_label, native_result, native_label);
        ctx.OpBranch(phase_merge_label);

        ctx.AddLabel(phase_merge_label);
        return result;
    }

    if (ctx.info.pgm_hash == DreamsCompat::SculptVolumeWriterShader) {
        // 84aa has one ordered-count operation in one wave per x workgroup. A collect dispatch
        // records its 0/1 contribution and terminates here. A tiny host compute pass then writes
        // every workgroup's exclusive prefix. Replay enters through the synthetic post-DOC merge,
        // so this block only supplies the phase-native result used by unsupported dispatch shapes.
        ASSERT_MSG(Sirit::ValidId(ctx.dreams_ordered_phase),
                   "Dreams 84aa DOC has no phase specialization constant");
        const Id phase = ctx.dreams_ordered_phase;
        const Id workgroup_x =
            ctx.OpCompositeExtract(ctx.U32[1], ctx.OpLoad(ctx.U32[3], ctx.workgroup_id), 0);
        const Id scratch_entry = ctx.OpIAdd(
            ctx.U32[1],
            ctx.ConstU32(DreamsCompat::OrderedScratchStreamBaseDword(
                DreamsCompat::SculptOrderedScratchStream)),
            ctx.OpIMul(ctx.U32[1], workgroup_x, ctx.ConstU32(DreamsCompat::OrderedEntryDwords)));

        ASSERT_MSG(Sirit::ValidId(ctx.dreams_sculpt_post_doc_label) &&
                       Sirit::ValidId(ctx.dreams_sculpt_replay_parent) &&
                       Sirit::ValidId(ctx.dreams_sculpt_replay_doc_result),
                   "Dreams 84aa replay gate was not emitted before DOC");
        const Id collect_label = ctx.OpLabel();
        const Id native_label = ctx.OpLabel();
        const Id is_collect =
            ctx.OpIEqual(ctx.U1[1], phase, ctx.ConstU32(DreamsCompat::OrderedPhaseCollect));
        ctx.OpSelectionMerge(native_label, spv::SelectionControlMask::MaskNone);
        ctx.OpBranchConditional(is_collect, collect_label, native_label);

        ctx.AddLabel(collect_label);
        const Id store_label = ctx.OpLabel();
        const Id store_merge_label = ctx.OpLabel();
        ctx.OpSelectionMerge(store_merge_label, spv::SelectionControlMask::MaskNone);
        ctx.OpBranchConditional(is_leader, store_label, store_merge_label);

        ctx.AddLabel(store_label);
        const Id payload_index = ctx.OpIAdd(ctx.U32[1], scratch_entry,
                                            ctx.ConstU32(DreamsCompat::SculptOrderedPayloadOffset));
        const Id token_index = ctx.OpIAdd(ctx.U32[1], scratch_entry,
                                          ctx.ConstU32(DreamsCompat::SculptOrderedTokenOffset));
        ctx.OpStore(get_pointer(payload_index), value);
        ctx.OpStore(get_pointer(token_index), ordered_token);
        ctx.OpBranch(store_merge_label);

        ctx.AddLabel(store_merge_label);
        ctx.OpReturn();

        ctx.AddLabel(native_label);
        const Id native_delta = ctx.OpSelect(ctx.U32[1], is_leader, value, ctx.u32_zero_value);
        const Id native_previous = ctx.OpAtomicIAdd(ctx.U32[1], counter_ptr, device_scope,
                                                    acquire_release_semantics, native_delta);
        const Id native_result =
            ctx.OpGroupNonUniformBroadcastFirst(ctx.U32[1], subgroup_scope, native_previous);
        ctx.OpBranch(ctx.dreams_sculpt_post_doc_label);

        ctx.AddLabel(ctx.dreams_sculpt_post_doc_label);
        for (u32 index = 0; index < 7; ++index) {
            const Id merged = ctx.OpPhi(ctx.U32[1], ctx.dreams_sculpt_replay_values[index],
                                        ctx.dreams_sculpt_replay_parent,
                                        ctx.dreams_sculpt_normal_values[index], native_label);
            ctx.dreams_sculpt_state_insts[index]->SetDefinition(merged);
        }
        const Id merged_exec = ctx.OpPhi(ctx.U1[1], ctx.dreams_sculpt_replay_values[7],
                                         ctx.dreams_sculpt_replay_parent,
                                         ctx.dreams_sculpt_normal_values[7], native_label);
        ctx.dreams_sculpt_state_insts[7]->SetDefinition(merged_exec);
        return ctx.OpPhi(ctx.U32[1], ctx.dreams_sculpt_replay_doc_result,
                         ctx.dreams_sculpt_replay_parent, native_result, native_label);
    }

    const u32 operation = control & 0x3;
    const bool is_add = operation == 0 || operation == 2;
    const bool ordered_turnstile = [&] {
        const char* setting = std::getenv("SHADPS4_DREAMS_ORDERED_COUNT_TURNSTILE");
        return ctx.info.pgm_hash == DreamsCompat::TraversalShader && setting != nullptr &&
               setting[0] == '1' && setting[1] == '\0';
    }();
    if (ordered_turnstile) {
        // The Dreams traversal shader has one 64-lane wave per x-only workgroup. Run a bounded
        // batch concurrently, but hold each wave at its ordered-count sequence until every lower
        // WorkgroupId.x has completed that sequence. All active lanes execute this loop together;
        // keeping the subgroup converged avoids the old leader-only spin/broadcast failure.
        const Id turn_ptr = get_pointer(ctx.ConstU32(DreamsCompat::OrderedBaseSlotsDword));
        const Id workgroup_id =
            ctx.OpCompositeExtract(ctx.U32[1], ctx.OpLoad(ctx.U32[3], ctx.workgroup_id), 0);
        const Id wait_header = ctx.OpLabel();
        const Id wait_continue = ctx.OpLabel();
        const Id wait_merge = ctx.OpLabel();
        ctx.OpBranch(wait_header);

        ctx.AddLabel(wait_header);
        ctx.OpLoopMerge(wait_merge, wait_continue, spv::LoopControlMask::MaskNone);
        const Id current_turn =
            ctx.OpAtomicLoad(ctx.U32[1], turn_ptr, device_scope, acquire_semantics);
        const Id is_current_wave = ctx.OpIEqual(ctx.U1[1], current_turn, workgroup_id);
        ctx.OpBranchConditional(is_current_wave, wait_merge, wait_continue);

        ctx.AddLabel(wait_continue);
        ctx.OpBranch(wait_header);

        ctx.AddLabel(wait_merge);
    }
    if (DreamsCompat::NeedsBranchlessOrderedCountAdd(ctx.info.pgm_hash) && is_add) {
        // These Dreams shaders use add-form DS_ORDERED_COUNT bases to index indirect commands or
        // compact visibility records. Keep every active invocation on the same control-flow path
        // through the atomic and broadcast: Vulkan does not require subgroup reconvergence at the
        // merge of the leader-only selection below. Followers add zero, so the first active
        // invocation (the elected leader) still returns the guest base.
        const Id delta = ctx.OpSelect(ctx.U32[1], is_leader, value, ctx.u32_zero_value);
        const Id previous = ctx.OpAtomicIAdd(ctx.U32[1], counter_ptr, device_scope,
                                             acquire_release_semantics, delta);
        if (ordered_turnstile && (control & 0x8) != 0) {
            // WAVE_DONE closes this wave's four-counter sequence and admits the next workgroup.
            // Followers add zero so the subgroup remains converged through both atomics.
            const Id turn_ptr = get_pointer(ctx.ConstU32(DreamsCompat::OrderedBaseSlotsDword));
            const Id turn_delta =
                ctx.OpSelect(ctx.U32[1], is_leader, ctx.u32_one_value, ctx.u32_zero_value);
            ctx.OpAtomicIAdd(ctx.U32[1], turn_ptr, device_scope, acquire_release_semantics,
                             turn_delta);
        }
        return ctx.OpGroupNonUniformBroadcastFirst(ctx.U32[1], subgroup_scope, previous);
    }

    const Id leader_label = ctx.OpLabel();
    const Id follower_label = ctx.OpLabel();
    const Id merge_label = ctx.OpLabel();
    ctx.OpSelectionMerge(merge_label, spv::SelectionControlMask::MaskNone);
    ctx.OpBranchConditional(is_leader, leader_label, follower_label);

    ctx.AddLabel(leader_label);
    const Id previous = [&] {
        switch (control & 0x3) {
        case 1:
            return ctx.OpAtomicExchange(ctx.U32[1], counter_ptr, device_scope,
                                        acquire_release_semantics, value);
        case 3:
            return ctx.OpAtomicIIncrement(ctx.U32[1], counter_ptr, device_scope,
                                          acquire_release_semantics);
        case 0:
        case 2:
        default:
            return ctx.OpAtomicIAdd(ctx.U32[1], counter_ptr, device_scope,
                                    acquire_release_semantics, value);
        }
    }();
    ctx.OpBranch(merge_label);

    ctx.AddLabel(follower_label);
    ctx.OpBranch(merge_label);

    ctx.AddLabel(merge_label);
    const Id leader_result =
        ctx.OpPhi(ctx.U32[1], previous, leader_label, ctx.u32_zero_value, follower_label);
    if (DreamsCompat::UseSculptOrderedCountLeaderBarrier(ctx.info.pgm_hash)) {
        // Experimental Dreams A/B: this makes the sculpt-volume writer's elected atomic result
        // visible before BroadcastFirst and improved tutorial/homespace geometry on CUSA04301.
        // It is opt-in because thousands of 64-lane workgroups make the barrier too expensive in
        // edit mode. Clear the cached 84aa3dc9 SPIR-V whenever this option is toggled.
        ctx.OpControlBarrier(subgroup_scope, subgroup_scope, acquire_release_semantics);
    }
    return ctx.OpGroupNonUniformBroadcastFirst(ctx.U32[1], subgroup_scope, leader_result);
}

} // namespace Shader::Backend::SPIRV
