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
constexpr u64 VisibilityListCompactShader = 0x7aa925e9;
constexpr u64 VisibilityListCompactShaderAlt = 0x016b9f6a;
constexpr u64 ProjectionConstantsShader = 0x934e15fc;
constexpr u64 SpatialReconstructionPrepareShader = 0x80aed032;
constexpr u64 SpatialReconstructionShader = 0xb1b1758b;
constexpr u64 TemporalResolveShader = 0x48a95ab7;
constexpr u64 SculptFragmentShader = 0x9d74c568;
constexpr u64 GatherVoxelsShader = 0x7ba4de5d;
// BUFFER_STORE records the PC after advancing past the 8-byte instruction at guest PC 0xdc8.
constexpr u32 ProjectionJitterStorePc = 0x0dd0;
constexpr u32 ProjectionJitterStoreOffset = 160;
constexpr u32 FrozenProjectionJitterX = 0x3840c27b;
constexpr u32 FrozenProjectionJitterY = 0x3b82aa30;
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

// Host-private GDS counters used by the opt-in GatherVoxels stage trace. The 8 MiB GDS backing
// contains 0x200000 dwords; ordered-count scratch ends at 0x104004, so this high range cannot
// overlap guest-visible GDS or ordered-count state.
constexpr u32 GatherStageTraceBaseDword = 0x1e0000;
constexpr u32 GatherStageTraceCount = 71;
// Keep numeric samples separate from the aggregate counters so a sampled register value cannot
// be mistaken for a stage count. Most samples come from workgroup zero; the tail records the
// first eight workgroups' candidate-buffer values and a validity mask.
constexpr u32 GatherNumericTraceBaseDword = GatherStageTraceBaseDword + 0x100;
constexpr u32 GatherNumericTraceCount = 46;
// Host-private counters for the opt-in d049fb84 model-finalize gate trace. Keep these well clear
// of both Gather tracing ranges. Numeric samples use eight dwords for the first active lane of
// each of the first eight workgroups: v7, v16, v8, shared ticket, value, threshold, iteration,
// and a validity marker.
constexpr u64 ModelFinalizeShader = 0xd049fb84;
constexpr u32 ModelFinalizeGateTraceBaseDword = 0x1e1000;
constexpr u32 ModelFinalizeGateTraceCount = 16;
constexpr u32 ModelFinalizeGateNumericBaseDword = ModelFinalizeGateTraceBaseDword + 0x100;
constexpr u32 ModelFinalizeGateNumericWorkgroups = 8;
constexpr u32 ModelFinalizeGateNumericStride = 8;
constexpr u32 ModelFinalizeGateNumericCount =
    ModelFinalizeGateNumericWorkgroups * ModelFinalizeGateNumericStride;
// Host-private trace storage shared by SceneCompact and its tiny post pass. The renderer clears
// and reads this range around each traced dispatch, so the two shaders can reuse it safely. The
// numeric area contains 16 compact-store pairs, eight compact-wave records, and 16 post ranges.
constexpr u64 ModelBucketPostShader = 0x14036f4e;
constexpr u32 ModelProducerTraceBaseDword = 0x1e2000;
constexpr u32 ModelProducerTraceCount = 32;
constexpr u32 ModelProducerNumericBaseDword = ModelProducerTraceBaseDword + 0x100;
constexpr u32 ModelProducerNumericCount = 160;
static_assert(GatherStageTraceBaseDword >= OrderedScratchBaseDword + OrderedScratchDwords);
static_assert(GatherStageTraceBaseDword + GatherStageTraceCount <= 0x200000);
static_assert(GatherStageTraceBaseDword + GatherStageTraceCount <=
              GatherNumericTraceBaseDword);
static_assert(GatherNumericTraceBaseDword + GatherNumericTraceCount <= 0x200000);
static_assert(GatherNumericTraceBaseDword + GatherNumericTraceCount <=
              ModelFinalizeGateTraceBaseDword);
static_assert(ModelFinalizeGateTraceBaseDword + ModelFinalizeGateTraceCount <=
              ModelFinalizeGateNumericBaseDword);
static_assert(ModelFinalizeGateNumericBaseDword + ModelFinalizeGateNumericCount <= 0x200000);
static_assert(ModelFinalizeGateNumericBaseDword + ModelFinalizeGateNumericCount <=
              ModelProducerTraceBaseDword);
static_assert(ModelProducerTraceBaseDword + ModelProducerTraceCount <=
              ModelProducerNumericBaseDword);
static_assert(ModelProducerNumericBaseDword + ModelProducerNumericCount <= 0x200000);

constexpr bool IsSpriteCullShader(u64 hash) {
    return hash == SpriteCullShader || hash == SpriteCullShaderAlt ||
           hash == SpriteCullShaderCusa04301;
}

constexpr bool NeedsBranchlessOrderedCountAdd(u64 hash) {
    return hash == IndirectArgsShader || hash == VisibilityListCompactShader ||
           hash == VisibilityListCompactShaderAlt;
}

constexpr bool NeedsLdsMemoryBarrier(u64 hash) {
    return hash == TemporalResolveShader;
}

constexpr bool NeedsPostFillLdsControlBarrier(u64 hash) {
    return hash == SpatialReconstructionPrepareShader || hash == SpatialReconstructionShader;
}

constexpr bool NeedsDreamsLdsBarrier(u64 hash) {
    return NeedsLdsMemoryBarrier(hash) || NeedsPostFillLdsControlBarrier(hash);
}

} // namespace Shader::DreamsCompat
