// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <unordered_set>
#include <vector>
#include "common/assert.h"
#include "shader_recompiler/dreams_compat.h"
#include "shader_recompiler/ir/breadth_first_search.h"
#include "shader_recompiler/ir/ir_emitter.h"
#include "shader_recompiler/ir/program.h"
#include "shader_recompiler/profile.h"

namespace Shader::Optimization {

static bool IsLoadShared(const IR::Inst& inst) {
    return inst.GetOpcode() == IR::Opcode::LoadSharedU16 ||
           inst.GetOpcode() == IR::Opcode::LoadSharedU32 ||
           inst.GetOpcode() == IR::Opcode::LoadSharedU64;
}

static bool IsWriteShared(const IR::Inst& inst) {
    return inst.GetOpcode() == IR::Opcode::WriteSharedU16 ||
           inst.GetOpcode() == IR::Opcode::WriteSharedU32 ||
           inst.GetOpcode() == IR::Opcode::WriteSharedU64;
}

// These reconstruction shaders exchange LDS values between lanes. An execution barrier would be
// invalid in their divergent regions, but the LDS dependency still requires a memory barrier.
// Insert exactly one immediately before the first LDS read after the shader's writes.
static void EmitDreamsLdsMemoryBarrier(IR::Program& program) {
    bool saw_shared_write{};
    for (const IR::AbstractSyntaxNode& node : program.syntax_list) {
        if (node.type != IR::AbstractSyntaxNode::Type::Block) {
            continue;
        }
        IR::Block* const block = node.data.block;
        for (IR::Inst& inst : block->Instructions()) {
            if (IsWriteShared(inst)) {
                saw_shared_write = true;
                continue;
            }
            if (!saw_shared_write || !IsLoadShared(inst)) {
                continue;
            }
            IR::IREmitter ir{*block, IR::Block::InstructionList::s_iterator_to(inst)};
            ir.WorkgroupMemoryBarrier();
            return;
        }
    }
}

// The spatial reconstruction shaders fill LDS in a divergent loop and reconverge at its merge.
// Synchronize there, where every lane executes the same dynamic barrier instance, before any lane
// starts consuming values written by another lane.
static void EmitDreamsPostFillLdsControlBarrier(IR::Program& program) {
    for (const IR::AbstractSyntaxNode& node : program.syntax_list) {
        if (node.type != IR::AbstractSyntaxNode::Type::Loop) {
            continue;
        }
        IR::Block* const merge = node.data.loop.merge;
        const auto insert_point = std::ranges::find_if_not(merge->Instructions(), IR::IsPhi);
        IR::IREmitter ir{*merge, insert_point};
        ir.Barrier();
        return;
    }
}

// GatherVoxels uses LDS as wave64 scratch in five producer phases. Liverpool's scalar waitcnts
// and wave issue order make every phase complete before the next one starts. The structured host
// shader needs that ordering stated explicitly, but a control barrier at a translated S_WAITCNT
// would be invalid wherever EXEC is divergent. Insert barriers only at the five uniform phase
// boundaries: once after the top-level prefill and once at each top-level producer-loop merge.
static void EmitDreamsGatherVoxelsLdsPhaseBarriers(IR::Program& program) {
    struct LoopState {
        IR::Block* merge{};
        u32 if_depth{};
        bool has_shared_write{};
    };

    using Type = IR::AbstractSyntaxNode::Type;
    u32 if_depth{};
    std::vector<LoopState> loop_stack;
    std::vector<IR::Block*> prefill_blocks;
    std::vector<IR::Block*> producer_loop_merges;

    for (const IR::AbstractSyntaxNode& node : program.syntax_list) {
        switch (node.type) {
        case Type::Block: {
            IR::Block* const block = node.data.block;
            const bool has_shared_write =
                std::ranges::any_of(block->Instructions(), IsWriteShared);
            if (!has_shared_write) {
                break;
            }
            for (LoopState& loop : loop_stack) {
                loop.has_shared_write = true;
            }
            if (if_depth == 0 && loop_stack.empty()) {
                prefill_blocks.push_back(block);
            }
            break;
        }
        case Type::If:
            ++if_depth;
            break;
        case Type::EndIf:
            ASSERT_MSG(if_depth != 0, "GatherVoxels LDS barrier scan has unmatched EndIf");
            --if_depth;
            break;
        case Type::Loop:
            loop_stack.push_back({.merge = node.data.loop.merge, .if_depth = if_depth});
            break;
        case Type::Repeat: {
            ASSERT_MSG(!loop_stack.empty(), "GatherVoxels LDS barrier scan has unmatched Repeat");
            const LoopState loop = loop_stack.back();
            loop_stack.pop_back();
            ASSERT_MSG(loop.merge == node.data.repeat.merge,
                       "GatherVoxels LDS barrier scan closed the wrong loop");
            if (loop.if_depth == 0 && loop.has_shared_write) {
                producer_loop_merges.push_back(loop.merge);
            }
            break;
        }
        default:
            break;
        }
    }

    ASSERT_MSG(if_depth == 0, "GatherVoxels LDS barrier scan has an unterminated If");
    ASSERT_MSG(loop_stack.empty(), "GatherVoxels LDS barrier scan has an unterminated loop");
    ASSERT_MSG(prefill_blocks.size() == 1,
               "GatherVoxels LDS barrier scan expected one top-level prefill, found {}",
               prefill_blocks.size());
    ASSERT_MSG(producer_loop_merges.size() == 4,
               "GatherVoxels LDS barrier scan expected four producer loops, found {}",
               producer_loop_merges.size());

    IR::IREmitter{*prefill_blocks.front()}.Barrier();
    for (IR::Block* const merge : producer_loop_merges) {
        const auto insert_point = std::ranges::find_if_not(merge->Instructions(), IR::IsPhi);
        IR::IREmitter{*merge, insert_point}.Barrier();
    }
}

// Inserts barriers when a shared memory write and read occur in the same basic block.
static void EmitBarrierInBlock(IR::Block* block) {
    enum class BarrierAction : u32 {
        None,
        BarrierOnWrite,
        BarrierOnRead,
    };
    BarrierAction action{};
    for (IR::Inst& inst : block->Instructions()) {
        if (IsLoadShared(inst)) {
            if (action == BarrierAction::BarrierOnRead) {
                IR::IREmitter ir{*block, IR::Block::InstructionList::s_iterator_to(inst)};
                ir.Barrier();
            }
            action = BarrierAction::BarrierOnWrite;
            continue;
        }
        if (IsWriteShared(inst)) {
            if (action == BarrierAction::BarrierOnWrite) {
                IR::IREmitter ir{*block, IR::Block::InstructionList::s_iterator_to(inst)};
                ir.Barrier();
            }
            action = BarrierAction::BarrierOnRead;
        }
    }
    if (action != BarrierAction::None) {
        IR::IREmitter ir{*block, --block->end()};
        ir.Barrier();
    }
}

using NodeSet = std::unordered_set<const IR::Block*>;

// Inserts a barrier after divergent conditional blocks to avoid undefined
// behavior when some threads write and others read from shared memory.
static void EmitBarrierInMergeBlock(const IR::AbstractSyntaxNode::Data& data,
                                    NodeSet& divergence_end, u32& divergence_depth) {
    const IR::U1 cond = data.if_node.cond;
    const auto is_divergent_cond =
        IR::BreadthFirstSearch(cond, [](IR::Inst* inst) -> std::optional<bool> {
            if (inst->GetOpcode() == IR::Opcode::GetAttributeU32 &&
                inst->Arg(0).Attribute() == IR::Attribute::LocalInvocationId) {
                return true;
            }
            return std::nullopt;
        });
    if (is_divergent_cond) {
        if (divergence_depth == 0) {
            IR::Block* const merge = data.if_node.merge;
            auto insert_point = std::ranges::find_if_not(merge->Instructions(), IR::IsPhi);
            IR::IREmitter ir{*merge, insert_point};
            ir.Barrier();
        }
        ++divergence_depth;
        divergence_end.emplace(data.if_node.merge);
    }
}

static constexpr u32 GcnSubgroupSize = 64;

void SharedMemoryBarrierPass(IR::Program& program, const RuntimeInfo& runtime_info,
                             const Profile& profile) {
    if (program.info.stage != Stage::Compute) {
        return;
    }
    const auto& cs_info = runtime_info.cs_info;
    const u32 shared_memory_size = cs_info.shared_memory_size;
    const u32 threadgroup_size =
        cs_info.workgroup_size[0] * cs_info.workgroup_size[1] * cs_info.workgroup_size[2];
    // The compiler can only omit barriers when the local workgroup size is the same as the HW
    // subgroup.
    if (shared_memory_size == 0 || threadgroup_size != GcnSubgroupSize ||
        !profile.needs_lds_barriers) {
        return;
    }
    if (DreamsCompat::NeedsLdsMemoryBarrier(program.info.pgm_hash)) {
        EmitDreamsLdsMemoryBarrier(program);
        return;
    }
    if (DreamsCompat::NeedsGatherVoxelsLdsPhaseBarriers(program.info.pgm_hash)) {
        EmitDreamsGatherVoxelsLdsPhaseBarriers(program);
        return;
    }
    if (DreamsCompat::NeedsPostFillLdsControlBarrier(program.info.pgm_hash)) {
        EmitDreamsPostFillLdsControlBarrier(program);
        return;
    }
    using Type = IR::AbstractSyntaxNode::Type;
    u32 divergence_depth{};
    NodeSet divergence_end;
    for (const IR::AbstractSyntaxNode& node : program.syntax_list) {
        if (node.type == Type::EndIf) {
            if (divergence_end.contains(node.data.end_if.merge)) {
                --divergence_depth;
            }
            continue;
        }
        // Check if branch depth is zero, we don't want to insert barrier in potentially divergent
        // code.
        if (node.type == Type::If) {
            EmitBarrierInMergeBlock(node.data, divergence_end, divergence_depth);
            continue;
        }
        if (node.type == Type::Block && divergence_depth == 0) {
            EmitBarrierInBlock(node.data.block);
        }
    }
}

} // namespace Shader::Optimization
