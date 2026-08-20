// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include "common/types.h"

namespace Shader::DreamsCompat {

constexpr u64 TraversalShader = 0xb535c6c8;
constexpr u64 QueueProducerShader = 0x2bfebd3c;
constexpr u64 QueueProducerShaderAlt = 0x692f0f7f;
constexpr u64 CompactClassifyShader = 0xcbac06d2;
constexpr u64 CompactScatterShader = 0xd4532ff4;
constexpr u64 SceneCompactShader = 0x3937a849;
constexpr u64 SpriteCullShader = 0xfd2a2c3b;
constexpr u64 SpriteCullShaderAlt = 0xe4dcd599;
constexpr u64 SpriteCullShaderCusa04301 = 0x0ffa5e6b;
constexpr u64 IndirectArgsShader = 0x90272fc4;
constexpr u32 TraversalOutputCounterIndex = 2;
constexpr u32 TraversalCompactCounterIndex = 6;
constexpr u32 TraversalSecondaryCounterIndex = 4;
constexpr u32 QueueProducerCounterIndex = 0x508;
constexpr u32 TraversalCompletionIndex = 16383;

// The guest-visible GDS ends at 64 KiB. Keep the enlarged allocation for compatibility with
// existing caches; the traversal shader only needs its final guest dword as private state.
constexpr u32 GuestGdsDwords = 16384;
constexpr u32 MaxTraversalWorkgroups = 65536;
constexpr u32 OrderedCounterCount = 4;
constexpr u32 OrderedEntryDwords = 4;
constexpr u32 OrderedCounterStrideDwords = MaxTraversalWorkgroups * OrderedEntryDwords;
constexpr u32 OrderedScratchBaseDword = GuestGdsDwords;
constexpr u32 OrderedBaseSlotsDword =
    OrderedScratchBaseDword + OrderedCounterCount * OrderedCounterStrideDwords;
constexpr u32 OrderedScratchDwords =
    OrderedCounterCount * OrderedCounterStrideDwords + OrderedCounterCount;

constexpr u32 OrderedClearOffsetBytes = TraversalCompletionIndex * sizeof(u32);
constexpr u32 OrderedClearSizeBytes = sizeof(u32);

constexpr bool IsSpriteCullShader(u64 hash) {
    return hash == SpriteCullShader || hash == SpriteCullShaderAlt ||
           hash == SpriteCullShaderCusa04301;
}

} // namespace Shader::DreamsCompat
