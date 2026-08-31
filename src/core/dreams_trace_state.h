// SPDX-FileCopyrightText: Copyright 2024-2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <atomic>

#include "common/types.h"

namespace Core::DreamsTrace {

constexpr u32 PendingModelStatus = 0xffffffffu;

struct ModelBuildSnapshot {
    bool valid{};
    bool returned{};
    u32 epoch{};
    u32 thread_id{};
    u32 arg_rcx{};
    u32 arg_rdi{};
    u32 arg_rdx{};
    u32 model_id{};
    u32 peer_id{};
    u32 status{PendingModelStatus};
};

// The CPU model builder and GPU command processor run on different host threads. This tiny
// seqlock tags diagnostic GPU dispatches with the guest model build that submitted them without
// adding locks to either normal execution path. It is written only when the opt-in model-input
// trace has installed its two guest breakpoints.
inline std::atomic<u32> model_sequence{};
inline std::atomic_flag model_writer = ATOMIC_FLAG_INIT;
inline std::atomic<u32> model_epoch{};
inline std::atomic<u32> model_thread_id{};
inline std::atomic<u32> model_arg_rcx{};
inline std::atomic<u32> model_arg_rdi{};
inline std::atomic<u32> model_arg_rdx{};
inline std::atomic<u32> model_id{};
inline std::atomic<u32> model_peer_id{};
inline std::atomic<u32> model_status{PendingModelStatus};
inline std::atomic<bool> model_valid{};
inline std::atomic<bool> model_returned{};

inline void LockModelWriter() noexcept {
    while (model_writer.test_and_set(std::memory_order_acquire)) {
    }
}

inline void UnlockModelWriter() noexcept {
    model_writer.clear(std::memory_order_release);
}

inline u32 BeginModelBuild(const u32 thread_id, const u32 arg_rcx, const u32 arg_rdi,
                           const u32 arg_rdx, const u32 arg_r8, const u32 arg_r9) noexcept {
    LockModelWriter();
    model_sequence.fetch_add(1, std::memory_order_acq_rel);
    const u32 epoch = model_epoch.fetch_add(1, std::memory_order_relaxed) + 1;
    model_thread_id.store(thread_id, std::memory_order_relaxed);
    model_arg_rcx.store(arg_rcx, std::memory_order_relaxed);
    model_arg_rdi.store(arg_rdi, std::memory_order_relaxed);
    model_arg_rdx.store(arg_rdx, std::memory_order_relaxed);
    model_id.store(arg_r8, std::memory_order_relaxed);
    model_peer_id.store(arg_r9, std::memory_order_relaxed);
    model_status.store(PendingModelStatus, std::memory_order_relaxed);
    model_returned.store(false, std::memory_order_relaxed);
    model_valid.store(true, std::memory_order_relaxed);
    model_sequence.fetch_add(1, std::memory_order_release);
    UnlockModelWriter();
    return epoch;
}

inline bool CompleteModelBuild(const u32 epoch, const u32 thread_id, const u32 status) noexcept {
    LockModelWriter();
    if (!model_valid.load(std::memory_order_relaxed) ||
        model_epoch.load(std::memory_order_relaxed) != epoch ||
        model_thread_id.load(std::memory_order_relaxed) != thread_id) {
        UnlockModelWriter();
        return false;
    }
    model_sequence.fetch_add(1, std::memory_order_acq_rel);
    model_status.store(status, std::memory_order_relaxed);
    model_returned.store(true, std::memory_order_relaxed);
    model_sequence.fetch_add(1, std::memory_order_release);
    UnlockModelWriter();
    return true;
}

inline ModelBuildSnapshot ReadModelBuild() noexcept {
    for (u32 attempt = 0; attempt < 4; ++attempt) {
        const u32 before = model_sequence.load(std::memory_order_acquire);
        if ((before & 1) != 0) {
            continue;
        }
        ModelBuildSnapshot snapshot{
            .valid = model_valid.load(std::memory_order_relaxed),
            .returned = model_returned.load(std::memory_order_relaxed),
            .epoch = model_epoch.load(std::memory_order_relaxed),
            .thread_id = model_thread_id.load(std::memory_order_relaxed),
            .arg_rcx = model_arg_rcx.load(std::memory_order_relaxed),
            .arg_rdi = model_arg_rdi.load(std::memory_order_relaxed),
            .arg_rdx = model_arg_rdx.load(std::memory_order_relaxed),
            .model_id = model_id.load(std::memory_order_relaxed),
            .peer_id = model_peer_id.load(std::memory_order_relaxed),
            .status = model_status.load(std::memory_order_relaxed),
        };
        const u32 after = model_sequence.load(std::memory_order_acquire);
        if (before == after) {
            return snapshot;
        }
    }
    return {};
}

} // namespace Core::DreamsTrace
