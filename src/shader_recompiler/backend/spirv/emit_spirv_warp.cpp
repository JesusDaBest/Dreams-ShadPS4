// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "shader_recompiler/backend/spirv/emit_spirv_instructions.h"
#include "shader_recompiler/backend/spirv/spirv_emit_context.h"

namespace Shader::Backend::SPIRV {

Id SubgroupScope(EmitContext& ctx) {
    return ctx.ConstU32(static_cast<u32>(spv::Scope::Subgroup));
}

Id EmitWarpId(EmitContext& ctx) {
    UNREACHABLE();
}

Id EmitLaneId(EmitContext& ctx) {
    return ctx.OpLoad(ctx.U32[1], ctx.subgroup_local_invocation_id);
}

Id EmitQuadShuffle(EmitContext& ctx, Id value, Id index) {
    return ctx.OpGroupNonUniformQuadBroadcast(ctx.U32[1], SubgroupScope(ctx), value, index);
}

Id EmitShuffleXor(EmitContext& ctx, Id value, Id mask) {
    const Id scope = SubgroupScope(ctx);
    const Id lane_id = ctx.OpLoad(ctx.U32[1], ctx.subgroup_local_invocation_id);
    const Id target_lane = ctx.OpBitwiseXor(ctx.U32[1], lane_id, mask);
    const Id active_mask = ctx.OpGroupNonUniformBallot(ctx.U32[4], scope, ctx.true_value);
    const Id word_index =
        ctx.OpShiftRightLogical(ctx.U32[1], target_lane, ctx.ConstU32(5U));
    const Id bit_index = ctx.OpBitwiseAnd(ctx.U32[1], target_lane, ctx.ConstU32(31U));
    const Id active_word =
        ctx.OpVectorExtractDynamic(ctx.U32[1], active_mask, word_index);
    const Id target_bit = ctx.OpBitwiseAnd(
        ctx.U32[1], active_word,
        ctx.OpShiftLeftLogical(ctx.U32[1], ctx.ConstU32(1U), bit_index));
    const Id target_active =
        ctx.OpINotEqual(ctx.U1[1], target_bit, ctx.u32_zero_value);
    const Id safe_lane =
        ctx.OpSelect(ctx.U32[1], target_active, target_lane, lane_id);
    const Id shuffled =
        ctx.OpGroupNonUniformShuffle(ctx.U32[1], scope, value, safe_lane);
    return ctx.OpSelect(ctx.U32[1], target_active, shuffled, ctx.u32_zero_value);
}

Id EmitReadFirstLane(EmitContext& ctx, Id value) {
    return ctx.OpGroupNonUniformBroadcastFirst(ctx.U32[1], SubgroupScope(ctx), value);
}

Id EmitReadLane(EmitContext& ctx, Id value, Id lane) {
    return ctx.OpGroupNonUniformShuffle(ctx.U32[1], SubgroupScope(ctx), value, lane);
}

Id EmitWriteLane(EmitContext& ctx, Id value, Id write_value, u32 lane) {
    const Id lane_id{ctx.OpLoad(ctx.U32[1], ctx.subgroup_local_invocation_id)};
    const Id is_target_lane{ctx.OpIEqual(ctx.U1[1], lane_id, ctx.ConstU32(lane))};
    return ctx.OpSelect(ctx.U32[1], is_target_lane, write_value, value);
}

Id EmitBallot(EmitContext& ctx, Id bit) {
    return ctx.OpGroupNonUniformBallot(ctx.U32[4], SubgroupScope(ctx), bit);
}

Id EmitBallotFindLsb(EmitContext& ctx, Id mask) {
    return ctx.OpGroupNonUniformBallotFindLSB(ctx.U32[1], SubgroupScope(ctx), mask);
}

Id EmitGroupAny(EmitContext& ctx, Id bit) {
    return ctx.OpGroupNonUniformAny(ctx.U1[1], SubgroupScope(ctx), bit);
}

} // namespace Shader::Backend::SPIRV
