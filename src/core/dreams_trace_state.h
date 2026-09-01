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
    bool submission_fallback{};
    u32 epoch{};
    u32 thread_id{};
    u32 arg_rcx{};
    u32 arg_rdi{};
    u32 arg_rdx{};
    u32 model_id{};
    u32 peer_id{};
    u32 status{PendingModelStatus};
};

// The CPU model builder and GPU command processor run on different host threads. The seqlock
// preserves the latest diagnostic state for logging, while the thread-local snapshot is copied
// into GPU submissions made by the same guest thread. It is written only when an opt-in model
// boundary trace has installed its build/return breakpoints.
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
inline thread_local ModelBuildSnapshot active_model_build{};

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
    active_model_build = {
        .valid = true,
        .returned = false,
        .epoch = epoch,
        .thread_id = thread_id,
        .arg_rcx = arg_rcx,
        .arg_rdi = arg_rdi,
        .arg_rdx = arg_rdx,
        .model_id = arg_r8,
        .peer_id = arg_r9,
        .status = PendingModelStatus,
    };
    return epoch;
}

inline bool CompleteModelBuild(const u32 epoch, const u32 thread_id, const u32 status) noexcept {
    LockModelWriter();
    const bool matched = model_valid.load(std::memory_order_relaxed) &&
                         model_epoch.load(std::memory_order_relaxed) == epoch &&
                         model_thread_id.load(std::memory_order_relaxed) == thread_id;
    if (matched) {
        model_sequence.fetch_add(1, std::memory_order_acq_rel);
        model_status.store(status, std::memory_order_relaxed);
        model_returned.store(true, std::memory_order_relaxed);
        model_sequence.fetch_add(1, std::memory_order_release);
    }
    UnlockModelWriter();
    if (active_model_build.epoch == epoch && active_model_build.thread_id == thread_id) {
        active_model_build = {};
    }
    return matched;
}

inline ModelBuildSnapshot ReadActiveModelBuild() noexcept {
    return active_model_build;
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

// GPU submissions may be emitted by a worker other than the guest thread that entered the model
// builder. Prefer that thread's exact snapshot when available, then fall back to the seqlock's
// current in-progress build so the diagnostic tag crosses the submission-thread handoff. A
// completed build is never attached to later, unrelated submissions.
inline ModelBuildSnapshot ReadModelBuildForSubmission() noexcept {
    const ModelBuildSnapshot active = ReadActiveModelBuild();
    if (active.valid && !active.returned && active.epoch != 0) {
        return active;
    }
    if (!model_valid.load(std::memory_order_acquire)) {
        return {};
    }
    ModelBuildSnapshot current = ReadModelBuild();
    if (!current.valid || current.returned || current.epoch == 0) {
        return {};
    }
    current.submission_fallback = true;
    return current;
}

} // namespace Core::DreamsTrace
