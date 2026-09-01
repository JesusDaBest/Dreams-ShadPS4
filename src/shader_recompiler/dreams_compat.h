// SPDX-FileCopyrightText: Copyright 2026 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <array>
#include <cstdlib>

#include "common/types.h"

namespace Shader::DreamsCompat {

// AMD encodes both the M0 high-half ordered-count base and OFFSET0 in bytes. Keep every Dreams
// replay path on the same byte-to-dword conversion as the generic shader lowering.
constexpr u32 OrderedCounterDword(u32 m0_base_bytes, u32 control) {
    return ((m0_base_bytes & 0xfffc) + ((control >> 8) & 0xff)) >> 2;
}

constexpr u64 TraversalShader = 0xb535c6c8;
// Bump when b535 traversal lowering changes incompatibly. Revision 6 includes the exact
// unordered NGE comparison semantics used by its near-detail refinement path.
constexpr u32 TraversalCacheRevision = 10;
// The instruction microtrace is a separate, opt-in traversal permutation. Its revision is kept
// separate from the guest-visible traversal lowering so diagnostics never invalidate or replace
// the normal Dreams shader.
constexpr u32 B535InstructionTraceCacheRevision = 5;
constexpr u64 QueueProducerShader = 0x2bfebd3c;
constexpr u64 QueueProducerShaderAlt = 0x692f0f7f;
// Bump when the Dreams-only 692 queue-producer ordered-count lowering changes incompatibly.
constexpr u32 QueueProducerAltCacheRevision = 5;
constexpr u64 CompactClassifyShader = 0xcbac06d2;
constexpr u64 CompactScatterShader = 0xd4532ff4;
constexpr u64 SceneCompactShader = 0x3937a849;
// Bump when the Dreams-only 393 SceneCompact ordered-count lowering changes incompatibly.
constexpr u32 SceneCompactCacheRevision = 2;
constexpr u64 SpriteCullShader = 0xfd2a2c3b;
constexpr u64 SpriteCullShaderAlt = 0xe4dcd599;
constexpr u64 SpriteCullShaderCusa04301 = 0x0ffa5e6b;
constexpr u64 IndirectArgsShader = 0x90272fc4;
// Bump when the Dreams-only 902 ordered-count lowering changes incompatibly.
constexpr u32 IndirectArgsCacheRevision = 7;
// 90272fc4 uses one global instance-prefix ordered counter at dword 0xa6 and four per-row
// command-reservation counters at dwords 0xa7 through 0xaa. Their initial values are
// guest GDS state; the exact prefix pass must preserve them rather than inventing a shader-scoped
// reset.
constexpr u32 IndirectArgsOrderedGlobalCounterDword = 0xa6;
constexpr u32 IndirectArgsOrderedRowCounterFirstDword = 0xa7;
constexpr u32 IndirectArgsOrderedRowCounterStrideDwords = 1;
constexpr u64 VisibilityCandidateCompactShader = 0xd8b4ddb5;
// Bump when the Dreams-only d8 visibility-candidate ordered-count lowering changes incompatibly.
constexpr u32 VisibilityCandidateCompactCacheRevision = 7;
// Stable radix compactor which publishes the positional key stream consumed by GatherVoxels.
constexpr u64 GatherInputCompactShader = 0x95c28c88;
constexpr u32 GatherInputCompactCacheRevision = 6;
constexpr u64 VisibilityListCompactShader = 0x7aa925e9;
constexpr u64 VisibilityListCompactShaderAlt = 0x016b9f6a;
// Bump when the Dreams-only final visibility-list ordered-count lowering changes incompatibly.
constexpr u32 VisibilityListCompactCacheRevision = 7;
constexpr std::array<u32, 2> VisibilityListOrderedCounterIndices{0x15f, 0x160};
constexpr std::array<u32, 2> VisibilityListAltOrderedCounterIndices{0x161, 0x162};
constexpr u32 OrderedChainCaptureGdsDwords =
    VisibilityListAltOrderedCounterIndices.back() + 1;
constexpr u64 SpatialReconstructionPrepareShader = 0x80aed032;
constexpr u64 SpatialReconstructionShader = 0xb1b1758b;
constexpr u64 TemporalResolveShader = 0x48a95ab7;
constexpr u64 UnboundedLookupShader = 0x5d7ca3f8;
constexpr u64 UnboundedLookupShaderAlt = 0xbc1eb6df;
constexpr u64 SculptFragmentShader = 0x9d74c568;
constexpr u64 GatherVoxelsShader = 0x7ba4de5d;
// Bump when the Dreams-only GatherVoxels synchronization or lowering changes incompatibly.
// Revision 1 adds the five guest LDS phase boundaries that were previously discarded with
// S_WAITCNT, so stale cached modules must not bypass them.
constexpr u32 GatherVoxelsCacheRevision = 1;
// The three compute shaders which can write the 4 MiB B1 arena consumed by B535. Ordered-chain
// captures instrument their exact dynamic store sites so a relocated row can be followed without
// guessing from overlapping buffer descriptors.
constexpr u64 B1SeedWriterShader = 0x4ebeffd2;
// Bump when the Dreams-only B1 seed writer ordered-count lowering changes incompatibly.
constexpr u32 B1SeedWriterCacheRevision = 1;
constexpr u64 B1TransformWriterShader = 0xa3a9e9ef;
// Builds the key/payload and per-bucket count tables consumed by A3's child lookup.
constexpr u64 A3LookupProducerShader = 0x63ddac84;
constexpr u64 SculptVolumeWriterShader = 0x84aa3dc9;
// Bump when the Dreams-only 84aa lowering changes incompatibly. Keeping this revision targeted
// avoids invalidating every unrelated shader in a user's cache.
constexpr u32 SculptVolumeWriterCacheRevision = 10;
// Extracts surface records from the sculpt-density volume. Its terminal workgroup publishes the
// compact count, so the sole DS_ORDERED_COUNT must return prefixes in guest wave-creation order.
constexpr u64 SculptSurfaceCompactShader = 0xf030fdc4;
constexpr u32 SculptSurfaceCompactCacheRevision = 6;
// Appends the final surface records to f030's compact list and republishes its total count.
constexpr u64 SculptSurfaceFinalizeShader = 0x5ac53394;
constexpr u32 SculptSurfaceFinalizeCacheRevision = 2;
constexpr u64 Vs370InterfaceCaptureShader = 0x3706083c;
constexpr u64 Ce3ReadConstCaptureShader = 0xce3b8413;
constexpr u32 TraversalOutputCounterIndex = 2;
constexpr u32 TraversalCompactCounterIndex = 6;
constexpr u32 TraversalSecondaryCounterIndex = 4;
// QueueProducer's compact-list state and byte-addressed DS_ORDERED_COUNT counters converted to
// guest GDS dword indices.
constexpr u32 QueueProducerCompactBaseIndex = 320;
constexpr u32 QueueProducerCompactCountIndex = 321;
constexpr u32 QueueProducerPrimaryOrderedCounterIndex = 0x142;
constexpr u32 QueueProducerPrimaryOrderedControl = 0x000c;
constexpr u32 QueueProducerPrimaryLanesPerWorkgroup = 64;
constexpr std::array<u32, 2> QueueProducerAltOrderedCounterIndices{0x143, 0x145};
constexpr u32 TraversalCompletionIndex = 16383;

// 2bf computes its global record index as (WorkgroupId.x << 6) + LocalInvocationId.x. The aligned
// Dreams chain dispatches it as 1x1x1 groups of 64x1x1 threads, so its sole DS_ORDERED_COUNT is
// executed by exactly one Liverpool wave. Inter-wave creation order is therefore vacuous and the
// existing subgroup-leader atomic is the exact guest result; collect/prefix/replay would only run
// the shader's many pre-DOC output stores twice.
constexpr bool QueueProducerPrimaryHasOneWave(u32 dim_x, u32 dim_y, u32 dim_z, u32 threads_x,
                                              u32 threads_y, u32 threads_z) {
    return dim_x == 1 && dim_y == 1 && dim_z == 1 &&
           threads_x == QueueProducerPrimaryLanesPerWorkgroup && threads_y == 1 && threads_z == 1;
}
static_assert(QueueProducerPrimaryHasOneWave(1, 1, 1, 64, 1, 1));
static_assert(!QueueProducerPrimaryHasOneWave(2, 1, 1, 64, 1, 1));

// These two shaders receive a raw buffer descriptor whose UINT32_MAX record count disables the
// guest bounds check; it is not a request to allocate the rest of the guest address space. Their
// complete post-resource-tracking IR has only two address families on guest buffer zero:
//
//   scalar: (source & 15)                         -> dword 0..15
//   U32x3:  (((source & 15) * 12) + 64) / 4      -> dword 16..61
//
// The last U32x3 component is dword 63, so [0, 0x100) is the exact conservative byte range. This
// preserves every address either shader can issue while avoiding a fabricated multi-megabyte
// mirror for a descriptor that merely expresses unbounded guest access.
constexpr u32 UnboundedLookupBufferIndex = 0;
constexpr u64 UnboundedLookupReachableBytes = 0x100;
constexpr u64 ExactUnboundedBufferReachableBytes(u64 hash, u32 buffer_index) {
    return buffer_index == UnboundedLookupBufferIndex &&
                   (hash == UnboundedLookupShader || hash == UnboundedLookupShaderAlt)
               ? UnboundedLookupReachableBytes
               : 0;
}
static_assert(((15u * 12u + 64u) / sizeof(u32) + 3u) * sizeof(u32) ==
              UnboundedLookupReachableBytes);

// The guest-visible GDS ends at 64 KiB. Keep the enlarged allocation for compatibility with
// existing caches; the traversal shader only needs its final guest dword as private state.
constexpr u32 GuestGdsDwords = 16384;
constexpr u32 MaxTraversalWorkgroups = 65536;
// Scratch stream 0 is shared by 4eb and 84aa. 902 uses stream 1 for its global prefix and streams 2..5
// for the four row-local prefixes. Traversal reuses streams 0..3; d8 uses stream 6 across its ten
// sequential radix passes. Each stream is indexed by guest wave-creation order, never by
// host/Vulkan arrival order.
constexpr u32 OrderedCounterCount = 7;
constexpr u32 ExistingOrderedCounterCount = 6;
constexpr u32 OrderedEntryDwords = 4;
constexpr u32 OrderedCounterStrideDwords = MaxTraversalWorkgroups * OrderedEntryDwords;
constexpr u32 OrderedScratchBaseDword = GuestGdsDwords;
// Keep the six existing metadata slots fixed so cached b535/902/84aa modules retain their exact
// addresses. The new d8 stream is placed after those slots instead of shifting them.
constexpr u32 OrderedBaseSlotsDword =
    OrderedScratchBaseDword + ExistingOrderedCounterCount * OrderedCounterStrideDwords;
constexpr u32 OrderedScratchStreamBaseDword(u32 stream) {
    return stream < ExistingOrderedCounterCount
               ? OrderedScratchBaseDword + stream * OrderedCounterStrideDwords
               : OrderedBaseSlotsDword + ExistingOrderedCounterCount +
                     (stream - ExistingOrderedCounterCount) * OrderedCounterStrideDwords;
}
constexpr u32 OrderedScratchDwords =
    ExistingOrderedCounterCount * OrderedCounterStrideDwords + ExistingOrderedCounterCount +
    (OrderedCounterCount - ExistingOrderedCounterCount) * (OrderedCounterStrideDwords + 1);

// 4eb has one 64-lane guest wave per x workgroup and reaches its sole DS_ORDERED_COUNT before
// any guest-visible store. Its returned prefix selects the B1 output allocation, so preserve
// Liverpool's x-fast wave-creation order with collect/prefix/replay on shared scratch stream zero.
constexpr u32 B1SeedWriterOrderedCounterIndex = 0x79;
constexpr u32 B1SeedWriterOrderedControl = 0x0004000c;
constexpr u32 B1SeedWriterOrderedLanesPerWorkgroup = 64;
constexpr u32 B1SeedWriterOrderedScratchStream = 0;
constexpr u32 B1SeedWriterOrderedScratchBaseDword =
    OrderedScratchStreamBaseDword(B1SeedWriterOrderedScratchStream);
static_assert(B1SeedWriterOrderedScratchStream < OrderedCounterCount);

// b535 has one guest wave per workgroup and four ordered-count operations before its first
// guest-visible store. The controls encode OFFSET0 and the add/WAVE_DONE bits; resource tracking
// maps those byte offsets onto the four GDS dword counters below. Collect records all four payloads
// in full x-fast WorkgroupIndex order, then the prefix pass and a single replay reproduce Liverpool
// ordering without serializing workgroups.
constexpr u32 TraversalOrderedDocCount = 4;
constexpr std::array<u32, TraversalOrderedDocCount> TraversalOrderedControls{
    0x0804, 0x1804, 0x0c04, 0x100c};
constexpr std::array<u32, TraversalOrderedDocCount> TraversalOrderedCounterIndices{
    0x142, 0x146, 0x143, 0x144};
constexpr std::array<u32, TraversalOrderedDocCount> TraversalOrderedScratchStreams{0, 1, 2, 3};
constexpr u32 TraversalOrderedCounterBaseBytes = 0x500;
constexpr u32 TraversalOrderedCounterFromControl(u32 control) {
    return OrderedCounterDword(TraversalOrderedCounterBaseBytes, control);
}
constexpr u32 TraversalOrderedScratchBaseDword(u32 doc_index) {
    return OrderedScratchStreamBaseDword(TraversalOrderedScratchStreams[doc_index]);
}
// The host clears this private word before an indirect collect. For the supported y=z=1 shape,
// workgroup zero publishes NumWorkgroups.x (the full product) so the prefix pass needs no readback.
constexpr u32 TraversalOrderedGroupCountDword = OrderedBaseSlotsDword;
static_assert(TraversalOrderedScratchStreams.back() < OrderedCounterCount);
static_assert(TraversalOrderedCounterFromControl(TraversalOrderedControls[0]) ==
              TraversalOrderedCounterIndices[0]);
static_assert(TraversalOrderedCounterFromControl(TraversalOrderedControls[1]) ==
              TraversalOrderedCounterIndices[1]);
static_assert(TraversalOrderedCounterFromControl(TraversalOrderedControls[2]) ==
              TraversalOrderedCounterIndices[2]);
static_assert(TraversalOrderedCounterFromControl(TraversalOrderedControls[3]) ==
              TraversalOrderedCounterIndices[3]);

// 692 has one guest wave per x workgroup and makes both queue reservations before its first
// guest-visible store. Collect records both payloads in guest workgroup-creation order, two
// scans reproduce the ordered-count return values, and replay executes the output stores once.
// The streams intentionally reuse traversal scratch; command-buffer barriers order that reuse.
constexpr u32 QueueProducerAltOrderedDocCount = 2;
constexpr std::array<u32, QueueProducerAltOrderedDocCount> QueueProducerAltOrderedControls{
    0x0c04, 0x140c};
constexpr std::array<u32, QueueProducerAltOrderedDocCount> QueueProducerAltOrderedScratchStreams{
    0, 1};
constexpr u32 QueueProducerAltOrderedCounterBaseBytes = 0x500;
constexpr u32 QueueProducerAltOrderedGroupCountDword = TraversalOrderedGroupCountDword;
constexpr u32 QueueProducerAltOrderedLanesPerWorkgroup = 64;
constexpr u32 QueueProducerAltOrderedCounterFromControl(u32 control) {
    return OrderedCounterDword(QueueProducerAltOrderedCounterBaseBytes, control);
}
constexpr u32 QueueProducerAltOrderedScratchBaseDword(u32 doc_index) {
    return OrderedScratchStreamBaseDword(QueueProducerAltOrderedScratchStreams[doc_index]);
}
static_assert(QueueProducerAltOrderedScratchStreams.back() < OrderedCounterCount);
static_assert(QueueProducerAltOrderedCounterFromControl(QueueProducerAltOrderedControls[0]) ==
              QueueProducerAltOrderedCounterIndices[0]);
static_assert(QueueProducerAltOrderedCounterFromControl(QueueProducerAltOrderedControls[1]) ==
              QueueProducerAltOrderedCounterIndices[1]);

// The sculpt-volume writer has one wave per x workgroup and one ordered-count operation. Its
// collect pass stores one entry per full WorkgroupId.x; the prefix pass writes the exact guest
// return value beside it for the parallel replay pass.
constexpr u32 SculptOrderedCounterIndex = 0x78;
constexpr u32 SculptOrderedScratchStream = 0;
constexpr u32 SculptOrderedPayloadOffset = 0;
constexpr u32 SculptOrderedPrefixOffset = 1;
constexpr u32 SculptOrderedTokenOffset = 2;
constexpr u32 OrderedPhaseNative = 0;
constexpr u32 OrderedPhaseCollect = 1;
constexpr u32 OrderedPhaseReplay = 2;

// f030 dispatches one 4x4x4 Liverpool wave per workgroup. Reuse scratch stream zero only while its
// direct dispatch is enclosed by explicit reuse barriers; the host scan seeds from guest GDS[0x100].
constexpr u32 SculptSurfaceOrderedControl = 0x0005000c;
constexpr u32 SculptSurfaceOrderedCounterIndex = 0x100;
constexpr u32 SculptSurfaceOrderedScratchStream = 0;
constexpr u32 SculptSurfaceOrderedThreadsX = 4;
constexpr u32 SculptSurfaceOrderedThreadsY = 4;
constexpr u32 SculptSurfaceOrderedThreadsZ = 4;
constexpr u32 SculptSurfaceOrderedLanesPerWorkgroup =
    SculptSurfaceOrderedThreadsX * SculptSurfaceOrderedThreadsY *
    SculptSurfaceOrderedThreadsZ;
constexpr u32 SculptSurfaceOrderedScratchBaseDword =
    OrderedScratchStreamBaseDword(SculptSurfaceOrderedScratchStream);
// 5ac continues f030's GDS[0x100] counter and uses the same one-wave 4x4x4 workgroup shape. Its
// GDS descriptor is binding three, so its packed DOC control differs from f030's binding five.
constexpr u32 SculptSurfaceFinalizeOrderedControl = 0x0003000c;
constexpr u32 SculptSurfaceFinalizeOrderedCounterIndex = 0x100;
constexpr u32 SculptSurfaceFinalizeOrderedThreadsX = 4;
constexpr u32 SculptSurfaceFinalizeOrderedThreadsY = 4;
constexpr u32 SculptSurfaceFinalizeOrderedThreadsZ = 4;
constexpr u32 SculptSurfaceFinalizeOrderedLanesPerWorkgroup =
    SculptSurfaceFinalizeOrderedThreadsX * SculptSurfaceFinalizeOrderedThreadsY *
    SculptSurfaceFinalizeOrderedThreadsZ;
// CIK COMPUTE_DISPATCH_INITIATOR bit 3 enables ordered-append wave IDs. Bit 6 permits
// out-of-order wave launch, which cannot satisfy DS_ORDERED_COUNT's creation-order contract.
constexpr u32 DispatchInitiatorOrderedAppendEnable = 1u << 3;
constexpr u32 DispatchInitiatorOutOfOrderLaunch = 1u << 6;
constexpr bool HasOrderedWaveLaunch(u32 dispatch_initiator) {
    return (dispatch_initiator & DispatchInitiatorOrderedAppendEnable) != 0 &&
           (dispatch_initiator & DispatchInitiatorOutOfOrderLaunch) == 0;
}
// Exact ordered-count modules expose their phase as a Vulkan specialization constant. Each
// dispatch binds a
// separately compiled phase so unreachable collect/replay control flow is removed before GPU ISA
// generation instead of consuming registers in every invocation.
constexpr u32 OrderedPhaseSpecId = 0;
// Sirit interns ordinary scalar constants. Use a value that cannot alias any valid phase while the
// declaration is being assembled, then change that declaration to OpSpecConstant. All three
// Vulkan pipelines explicitly specialize it to one of the valid values above.
constexpr u32 OrderedPhaseUnspecialized = 0x44525048;

constexpr u32 IndirectArgsOrderedGlobalScratchStream = 1;
constexpr u32 IndirectArgsOrderedRowScratchFirstStream = 2;
constexpr u32 IndirectArgsOrderedRowCount = 4;
constexpr u32 IndirectArgsOrderedLanesPerWorkgroup = 64;
constexpr u32 IndirectArgsOrderedGlobalScratchBaseDword =
    OrderedScratchStreamBaseDword(IndirectArgsOrderedGlobalScratchStream);
constexpr u32 IndirectArgsOrderedRowScratchBaseDword(u32 row) {
    return OrderedScratchStreamBaseDword(IndirectArgsOrderedRowScratchFirstStream + row);
}
constexpr u32 IndirectArgsOrderedRowCounterDword(u32 row) {
    return IndirectArgsOrderedRowCounterFirstDword +
           row * IndirectArgsOrderedRowCounterStrideDwords;
}
static_assert(IndirectArgsOrderedRowScratchFirstStream + IndirectArgsOrderedRowCount <=
              OrderedCounterCount);
static_assert(IndirectArgsOrderedRowCounterDword(0) == 0xa7);
static_assert(IndirectArgsOrderedRowCounterDword(3) == 0xaa);

// d8 is a ten-pass ping-pong radix compactor. Every pass contains one DOC with packed control
// 0x0002000c, and its counter is GDS dword 0x155 + pass. Collect publishes the indirect x
// dimension for the x-by-1-by-1 wave64 dispatch and records one payload per WorkgroupIndex. The
// host scans the pass-selected counter, then replay performs the sole guest-visible output store
// exactly once.
constexpr u32 VisibilityCandidateOrderedPassCount = 10;
constexpr u32 VisibilityCandidateOrderedCounterFirstDword = 0x155;
constexpr u32 VisibilityCandidateOrderedCounterStrideDwords = 1;
constexpr u32 VisibilityCandidateOrderedScratchStream = 6;
constexpr u32 VisibilityCandidateOrderedScratchBaseDword =
    OrderedScratchStreamBaseDword(VisibilityCandidateOrderedScratchStream);
constexpr u32 VisibilityCandidateOrderedGroupCountDword =
    VisibilityCandidateOrderedScratchBaseDword + OrderedCounterStrideDwords;
constexpr u32 VisibilityCandidateOrderedControl = 0x0002000c;
constexpr u32 VisibilityCandidateOrderedLanesPerWorkgroup = 64;
constexpr u32 VisibilityCandidateOrderedCounterDword(u32 pass) {
    return VisibilityCandidateOrderedCounterFirstDword +
           pass * VisibilityCandidateOrderedCounterStrideDwords;
}
static_assert(VisibilityCandidateOrderedScratchStream < OrderedCounterCount);
static_assert(VisibilityCandidateOrderedCounterDword(0) == 0x155);
static_assert(VisibilityCandidateOrderedCounterDword(9) == 0x15e);
static_assert(TraversalOrderedGroupCountDword == 0x184000);

// 95c performs one stable radix pass per direct x-by-1-by-1 wave64 dispatch. Its sole DOC uses
// GDS[0x5d + pass], where pass is SRT word 9 / flattened user-data word 25. The only
// guest-visible
// store follows DOC, so collect/prefix/replay is exact without checkpointing any other state. It
// reuses d8's scratch stream; the host inserts a reuse barrier before every collect.
constexpr u32 GatherInputOrderedPassCount = 37;
constexpr u32 GatherInputOrderedCounterFirstDword = 0x5d;
constexpr u32 GatherInputOrderedCounterStrideDwords = 1;
constexpr u32 GatherInputOrderedControl = 0x0002000c;
constexpr u32 GatherInputOrderedScratchStream = VisibilityCandidateOrderedScratchStream;
constexpr u32 GatherInputOrderedScratchBaseDword =
    OrderedScratchStreamBaseDword(GatherInputOrderedScratchStream);
constexpr u32 GatherInputOrderedLanesPerWorkgroup = 64;
constexpr u32 GatherInputOrderedCounterDword(u32 pass) {
    return GatherInputOrderedCounterFirstDword + pass * GatherInputOrderedCounterStrideDwords;
}
static_assert(GatherInputOrderedScratchStream < OrderedCounterCount);
static_assert(GatherInputOrderedCounterDword(GatherInputOrderedPassCount - 1) < GuestGdsDwords);

// 7aa/016 each compact the final visibility list into two categories. Both DOC reservations occur
// before either output store, so one collect can publish both workgroup payloads, two prefix scans
// can reproduce Liverpool's x-fast creation order, and one replay can execute both stores. These
// streams intentionally reuse traversal scratch: the dispatches are ordered by explicit barriers
// and the 8 MiB GDS allocation cannot hold two additional maximum-sized streams.
constexpr u32 VisibilityListOrderedDocCount = 2;
constexpr std::array<u32, VisibilityListOrderedDocCount> VisibilityListOrderedControls{0x0004,
                                                                                       0x040c};
constexpr std::array<u32, VisibilityListOrderedDocCount> VisibilityListOrderedScratchStreams{0,
                                                                                             1};
constexpr u32 VisibilityListOrderedGroupCountDword = TraversalOrderedGroupCountDword;
constexpr u32 VisibilityListOrderedLanesPerWorkgroup = 64;
constexpr u32 VisibilityListOrderedScratchBaseDword(u32 doc_index) {
    return OrderedScratchStreamBaseDword(VisibilityListOrderedScratchStreams[doc_index]);
}
constexpr const std::array<u32, VisibilityListOrderedDocCount>& VisibilityListOrderedCounters(
    u64 hash) {
    return hash == VisibilityListCompactShaderAlt ? VisibilityListAltOrderedCounterIndices
                                                   : VisibilityListOrderedCounterIndices;
}
static_assert(VisibilityListOrderedScratchStreams.back() < OrderedCounterCount);

// 84aa compacts Image #0/#1 and Buffer #0 in place. Seven U32 phis cross its sole DOC instruction.
// The first four are consumed only by lanes 0..31, while the remaining three are consumed by all
// 64 lanes. The lane-active phi is identically true in this exact shader (the valid path recovers
// each invocation's own bit from ballot(true), and the invalid path supplies true), so it does not
// need a checkpoint slot. Components remain contiguous lane arrays for coalesced access. Replay
// restores them at a synthetic post-DOC merge, bypassing every read of an in-place source. The
// first 8 MiB retain all older guest/private GDS ranges.
constexpr u32 SculptOrderedStateBaseDword = 0x200000;
constexpr u32 SculptOrderedLanesPerWorkgroup = 64;
constexpr u32 SculptOrderedLowerHalfLanes = SculptOrderedLanesPerWorkgroup / 2;
constexpr u32 SculptOrderedLowerHalfStateComponents = 4;
constexpr u32 SculptOrderedFullStateComponents = 3;
constexpr u32 SculptOrderedStateValueCount =
    SculptOrderedLowerHalfStateComponents + SculptOrderedFullStateComponents;
constexpr u32 SculptOrderedLowerHalfStateDwords =
    SculptOrderedLowerHalfStateComponents * SculptOrderedLowerHalfLanes;
constexpr u32 SculptOrderedFullStateDwords =
    SculptOrderedFullStateComponents * SculptOrderedLanesPerWorkgroup;
constexpr u32 SculptOrderedStateDwordsPerWorkgroup =
    SculptOrderedLowerHalfStateDwords + SculptOrderedFullStateDwords;
constexpr u32 SculptOrderedStateComponentOffset(u32 component) {
    return component < SculptOrderedLowerHalfStateComponents
               ? component * SculptOrderedLowerHalfLanes
               : SculptOrderedLowerHalfStateDwords +
                     (component - SculptOrderedLowerHalfStateComponents) *
                         SculptOrderedLanesPerWorkgroup;
}
constexpr u32 SculptOrderedMaxWorkgroups = 12288;
constexpr u32 SculptOrderedStateDwords =
    SculptOrderedStateDwordsPerWorkgroup * SculptOrderedMaxWorkgroups;
static_assert(SculptOrderedLanesPerWorkgroup % 2 == 0);
static_assert(SculptOrderedStateValueCount == 7);
static_assert(SculptOrderedStateComponentOffset(0) == 0);
static_assert(SculptOrderedStateComponentOffset(4) == SculptOrderedLowerHalfStateDwords);
static_assert(SculptOrderedStateComponentOffset(SculptOrderedStateValueCount - 1) +
                  SculptOrderedLanesPerWorkgroup ==
              SculptOrderedStateDwordsPerWorkgroup);
static_assert(SculptOrderedStateBaseDword + SculptOrderedStateDwords <= 0x800000);

// 393 has sixteen 64-lane Liverpool waves in each 1024-thread x-only workgroup. Every wave issues
// four ordered-count operations against its own four-counter row, while the same row is shared by
// all workgroups. Collect one payload per (wave, DOC, workgroup), scan all 64 streams in guest
// workgroup-creation order, then replay the shader once with the exact returned prefixes. Its
// ordered token is eleven bits, so keep the exact path within that non-wrapping domain. This range
// intentionally reuses the beginning of 84aa's checkpoint storage; command-buffer barriers order
// the two exact paths, and their dispatches never execute concurrently.
constexpr u32 SceneCompactOrderedLanesPerWave = 64;
constexpr u32 SceneCompactOrderedWavesPerWorkgroup = 16;
constexpr u32 SceneCompactOrderedThreadsPerWorkgroup =
    SceneCompactOrderedLanesPerWave * SceneCompactOrderedWavesPerWorkgroup;
constexpr u32 SceneCompactOrderedDocCount = 4;
constexpr u32 SceneCompactOrderedStreamCount =
    SceneCompactOrderedWavesPerWorkgroup * SceneCompactOrderedDocCount;
constexpr std::array<u32, SceneCompactOrderedDocCount> SceneCompactOrderedControls{
    0x0004, 0x0404, 0x0804, 0x0c0c};
constexpr u32 SceneCompactOrderedCounterBaseDword = 0x900;
constexpr u32 SceneCompactOrderedCounterRowStrideDwords = 4;
constexpr u32 SceneCompactOrderedMaxWorkgroups = 1u << 11;
constexpr u32 SceneCompactOrderedScratchBaseDword = SculptOrderedStateBaseDword;
constexpr u32 SceneCompactOrderedScratchStreamStrideDwords =
    SceneCompactOrderedMaxWorkgroups * OrderedEntryDwords;
constexpr u32 SceneCompactOrderedScratchDwords =
    SceneCompactOrderedStreamCount * SceneCompactOrderedScratchStreamStrideDwords;
constexpr u32 SceneCompactOrderedCounterDword(u32 stream) {
    return SceneCompactOrderedCounterBaseDword +
           (stream / SceneCompactOrderedDocCount) * SceneCompactOrderedCounterRowStrideDwords +
           stream % SceneCompactOrderedDocCount;
}
constexpr u32 SceneCompactOrderedScratchStreamBaseDword(u32 stream) {
    return SceneCompactOrderedScratchBaseDword +
           stream * SceneCompactOrderedScratchStreamStrideDwords;
}
static_assert(SceneCompactOrderedThreadsPerWorkgroup == 1024);
static_assert(SceneCompactOrderedStreamCount == 64);
static_assert(SceneCompactOrderedCounterDword(SceneCompactOrderedStreamCount - 1) <
              GuestGdsDwords);
static_assert(SceneCompactOrderedScratchBaseDword + SceneCompactOrderedScratchDwords <=
              SculptOrderedStateBaseDword + SculptOrderedStateDwords);

// Opt-in, one-shot membership trace for b535's sculpt traversal. The trace occupies the unused
// tail of the 32 MiB host-private GDS backing after 84aa's exact replay checkpoint. It is compiled
// only when SHADPS4_DREAMS_B535_MEMBERSHIP_CAPTURE=1, and the host arms it for one traversal chain
// with a trigger file. Fixed pass/workgroup/lane slots avoid an instrumentation-side allocator or
// any new ordering dependency in the guest shader.
namespace B535MembershipCapture {
constexpr u32 BaseDword = SculptOrderedStateBaseDword + SculptOrderedStateDwords;
constexpr u32 HeaderDwords = 0x40;
constexpr u32 ArmDword = BaseDword;
constexpr u32 PassDword = BaseDword + 1;
constexpr u32 OverflowDword = BaseDword + 2;
constexpr u32 RecordBaseDword = BaseDword + HeaderDwords;
constexpr u32 MaxPasses = 10;
// The isolated sculpt chain currently dispatches one x workgroup. Keep a guarded 64-workgroup
// envelope so every lane can carry the complete branch snapshot while remaining inside the
// existing 32 MiB private-GDS allocation. Overflow is explicit; it never silently truncates.
constexpr u32 MaxWorkgroupsPerPass = 64;
constexpr u32 LanesPerWorkgroup = 64;
constexpr u32 RecordDwords = 52;

enum Field : u32 {
    Valid = 0,
    Pass = 1,
    Workgroup = 2,
    Lane = 3,
    Identity = 4,
    Record = 5,
    ObjectIndex = 6,
    Object95 = 7,
    Object96 = 8,
    Object97 = 9,
    Object36 = 10,
    Root65 = 11,
    Root66 = 12,
    Root67 = 13,
    LodT = 14,
    LodBranchMaskLo = 15,
    LodBranchMaskHi = 16,
    RejectV8 = 17,
    RejectV3 = 18,
    RejectV5 = 19,
    RejectV1 = 20,
    RejectV4 = 21,
    SatReject = 22,
    CoarseGateV2 = 23,
    NestedV19 = 24,
    NestedV39 = 25,
    NestedV33 = 26,
    NestedV5 = 27,
    NestedV1 = 28,
    NestedV2 = 29,
    NestedV9 = 30,
    ScoreInput = 31,
    ScoreScale = 32,
    ScoreSlope = 33,
    ScoreProduct = 34,
    RawScore = 35,
    Score = 36,
    CandidateActive = 37,
    OutputIndex = 38,
    SelectorLimit = 39,
    OcclusionResult = 40,
    OutputRole = 41,
    PairLevel = 42,
    ProjectedBound = 43,
    NgeAfcV4 = 44,
    NgeAfcV2 = 45,
    NgeB38V3 = 46,
    NgeB38V47 = 47,
    Flags = 48,
    Gate1OriginalVcc = 49,
    Gate1ForceApplied = 50,
    InputB1 = 51,
};

constexpr u32 InputValid = 1u << 0;
constexpr u32 ObjectValid = 1u << 1;
constexpr u32 LodValid = 1u << 2;
constexpr u32 LodFarBranch = 1u << 3;
constexpr u32 InitialGateReached = 1u << 4;
constexpr u32 InitialGatePassed = 1u << 5;
constexpr u32 RejectStageValid = 1u << 6;
constexpr u32 RejectAxis0Passed = 1u << 7;
constexpr u32 RejectAxis1Passed = 1u << 8;
constexpr u32 RejectAxis2Passed = 1u << 9;
constexpr u32 SubdivisionGate0Reached = 1u << 10;
constexpr u32 SubdivisionGate0Passed = 1u << 11;
constexpr u32 SubdivisionGate1Reached = 1u << 12;
constexpr u32 SubdivisionGate1Passed = 1u << 13;
constexpr u32 NestedStageValid = 1u << 14;
constexpr u32 ScoreStageValid = 1u << 15;
constexpr u32 TerminalStageValid = 1u << 16;
constexpr u32 ScoreNonzero = 1u << 17;
constexpr u32 SatAccepted = 1u << 18;
constexpr u32 OutputInRange = 1u << 19;
constexpr u32 FinalAccepted = 1u << 20;
constexpr u32 SelectorAccepted = 1u << 21;
constexpr u32 OcclusionStageValid = 1u << 22;
constexpr u32 OcclusionAccepted = 1u << 23;
constexpr u32 OutputRoleChildB6 = 1u << 24;
constexpr u32 OutputRoleTerminalB3 = 1u << 25;
constexpr u32 OutputRoleTerminalB4 = 1u << 26;
constexpr u32 OutputRoleTerminalB5 = 1u << 27;
constexpr u32 OutputRoleDrop = 1u << 28;
constexpr u32 LodNearBranch = 1u << 29;
constexpr u32 NgeAfcVcc = 1u << 30;
constexpr u32 NgeB38Vcc = 1u << 31;

constexpr u32 RecordDword(u32 pass, u32 workgroup, u32 lane, u32 field) {
    return RecordBaseDword +
           (((pass * MaxWorkgroupsPerPass + workgroup) * LanesPerWorkgroup + lane) *
                RecordDwords +
            field);
}

constexpr u32 RecordCapacity = MaxPasses * MaxWorkgroupsPerPass * LanesPerWorkgroup;
constexpr u32 DwordCount = HeaderDwords + RecordCapacity * RecordDwords;
} // namespace B535MembershipCapture
static_assert(B535MembershipCapture::RecordDwords == B535MembershipCapture::InputB1 + 1);
static_assert(B535MembershipCapture::BaseDword >=
              SculptOrderedStateBaseDword + SculptOrderedStateDwords);
static_assert(B535MembershipCapture::BaseDword + B535MembershipCapture::DwordCount <= 0x800000);

// Exact values consumed by every active lane of QueueProducer's decisive B1/B2 stores. The
// aligned Dreams dispatch is exactly one 64-lane Liverpool wave, so fixed lane slots preserve the
// complete producer handoff without ticket allocation, cross-lane collisions, or guest-visible
// state. This is intentionally adjacent to (and disjoint from) the ten-pass B535 membership trace
// so one ordered-chain capture can prove both sides of the handoff without relying on a
// potentially stale guest CPU mirror.
namespace QueueProducerCapture {
constexpr u32 BaseDword = B535MembershipCapture::BaseDword + B535MembershipCapture::DwordCount;
constexpr u32 Magic = 0x33465051; // "QPF3"
constexpr u32 Schema = 3;
constexpr u32 Lanes = 64;
constexpr u32 HeaderDwords = 16;
constexpr u32 ArmDword = BaseDword;
constexpr u32 MagicDword = BaseDword + 1;
constexpr u32 SchemaDword = BaseDword + 2;
constexpr u32 LanesDword = BaseDword + 3;
constexpr u32 SitesDword = BaseDword + 4;
constexpr u32 SeenMask0Dword = BaseDword + 5;
constexpr u32 SeenMask1Dword = BaseDword + 6;
constexpr u32 B1RecordDwords = 4;
enum B1Field : u32 {
    B1Valid = 0,
    B1Address = 1,
    B1Data0 = 2,
    B1Data1 = 3,
};
constexpr u32 B2Sites = 5;
constexpr u32 B2RecordDwords = 6;
enum B2Field : u32 {
    B2Valid = 0,
    B2Address = 1,
    B2Data0 = 2,
    B2Data1 = 3,
    B2Data2 = 4,
    B2Data3 = 5,
};
constexpr u32 B1RecordBaseDword = BaseDword + HeaderDwords;
constexpr u32 B2RecordBaseDword = B1RecordBaseDword + Lanes * B1RecordDwords;
constexpr u32 DwordCount =
    HeaderDwords + Lanes * (B1RecordDwords + B2Sites * B2RecordDwords);

constexpr u32 B1RecordDword(u32 lane, u32 field) {
    return B1RecordBaseDword + lane * B1RecordDwords + field;
}

constexpr u32 B2RecordDword(u32 lane, u32 site, u32 field) {
    return B2RecordBaseDword + (lane * B2Sites + site) * B2RecordDwords + field;
}
} // namespace QueueProducerCapture
static_assert(QueueProducerCapture::BaseDword >=
              B535MembershipCapture::BaseDword + B535MembershipCapture::DwordCount);
static_assert(QueueProducerCapture::BaseDword + QueueProducerCapture::DwordCount <= 0x800000);

// Strictly opt-in instruction-level trace for every active lane in B535 pass 4. Record identities
// are allocated by the guest and change between launches, so fixed workgroup/lane banks are the
// only restart-proof key. The dense PC window begins at the bound-data load and ends at the second
// NGE comparison. Unused PC slots are the second dword of 64-bit encodings. Each compact record
// preserves all four raw source operands, destination zero, and the lane's pre/post VCC value.
// Metadata is shared by all lanes. All storage is host-private GDS and no captured value feeds
// guest execution.
namespace B535InstructionTrace {
constexpr u32 AlignDwords = 0x100;
constexpr u32 BaseDword =
    (QueueProducerCapture::BaseDword + QueueProducerCapture::DwordCount + AlignDwords - 1) &
    ~(AlignDwords - 1);
constexpr u32 Magic = 0x42353335; // "B535"
constexpr u32 Schema = 2;
constexpr u32 TargetPass = 4;
constexpr u32 FirstPc = 0x920;
constexpr u32 LastPc = 0xb38;
constexpr u32 StepCount = (LastPc - FirstPc) / sizeof(u32) + 1;
constexpr u32 MaxWorkgroups = 2;
constexpr u32 LanesPerWorkgroup = 64;
constexpr u32 LaneBanks = MaxWorkgroups * LanesPerWorkgroup;

constexpr u32 HeaderDwords = 0x20;
enum HeaderField : u32 {
    HeaderMagic = 0,
    HeaderSchema = 1,
    HeaderTargetPass = 2,
    HeaderFirstPc = 3,
    HeaderLastPc = 4,
    HeaderStepCount = 5,
    HeaderMetadataDwords = 6,
    HeaderRecordDwords = 7,
    HeaderMaxWorkgroups = 8,
    HeaderLanesPerWorkgroup = 9,
    HeaderLaneBanks = 10,
    HeaderTraceRevision = 11,
    HeaderMetadataBase = 12,
    HeaderRecordBase = 13,
    HeaderSeenMask0 = 16,
    HeaderSeenMask1 = 17,
    HeaderSeenMask2 = 18,
    HeaderSeenMask3 = 19,
};

// Five dwords preserve an operand without depending on enum layouts in an offline checker:
// field, scalar type, raw decoder code, modifier bits, and an auxiliary modifier word. For a
// source the auxiliary word describes DPP; for a destination it is the exact multiplier bits.
constexpr u32 OperandMetadataDwords = 5;
constexpr u32 OperandCount = 6; // four sources followed by two destinations
constexpr u32 MetadataDwords = 48;
enum MetadataField : u32 {
    MetadataValid = 0,
    MetadataPc = 1,
    MetadataOpcode = 2,
    MetadataEncoding = 3,
    MetadataClass = 4,
    MetadataCategory = 5,
    MetadataLength = 6,
    MetadataSrcCount = 7,
    MetadataDstCount = 8,
    MetadataControlLo = 9,
    MetadataControlHi = 10,
    MetadataOperandBase = 11,
};
constexpr u32 MetadataBaseDword = BaseDword + HeaderDwords;

constexpr u32 RecordDwords = 15;
enum RecordField : u32 {
    Valid = 0,
    Identity = 1,
    Record = 2,
    PreVcc = 3,
    PreSrc0Lo = 4,
    PreSrc0Hi = 5,
    PreSrc1Lo = 6,
    PreSrc1Hi = 7,
    PreSrc2Lo = 8,
    PreSrc2Hi = 9,
    PreSrc3Lo = 10,
    PreSrc3Hi = 11,
    PostDst0Lo = 12,
    PostDst0Hi = 13,
    PostVcc = 14,
};
constexpr u32 PreValid = 1u << 0;
constexpr u32 PostValid = 1u << 1;
constexpr u32 RecordBaseDword = MetadataBaseDword + StepCount * MetadataDwords;
constexpr u32 DwordCount = HeaderDwords + StepCount * MetadataDwords +
                           LaneBanks * StepCount * RecordDwords;

constexpr bool ContainsPc(u32 pc) {
    return pc >= FirstPc && pc <= LastPc && ((pc - FirstPc) & 3) == 0;
}

constexpr u32 StepFromPc(u32 pc) {
    return (pc - FirstPc) / sizeof(u32);
}

constexpr u32 MetadataDword(u32 pc, u32 field) {
    return MetadataBaseDword + StepFromPc(pc) * MetadataDwords + field;
}

constexpr u32 OperandMetadataDword(u32 pc, u32 operand, u32 field) {
    return MetadataDword(pc, MetadataOperandBase + operand * OperandMetadataDwords + field);
}

constexpr u32 RecordDword(u32 bank, u32 pc, u32 field) {
    return RecordBaseDword + (bank * StepCount + StepFromPc(pc)) * RecordDwords + field;
}
} // namespace B535InstructionTrace
static_assert(B535InstructionTrace::MetadataOperandBase +
                  B535InstructionTrace::OperandCount *
                      B535InstructionTrace::OperandMetadataDwords <=
              B535InstructionTrace::MetadataDwords);
static_assert(B535InstructionTrace::PostVcc < B535InstructionTrace::RecordDwords);
static_assert(B535InstructionTrace::BaseDword >=
              QueueProducerCapture::BaseDword + QueueProducerCapture::DwordCount);
static_assert(B535InstructionTrace::BaseDword + B535InstructionTrace::DwordCount <= 0xc00000);

// Ordered-chain-only provenance for every possible row in B535 binding 10 (B1). Each writer
// stores the exact value it emitted and a tag identifying its static store site. A3 additionally
// stores the original v13 input plus the complete predicate flags from PC 0x1f04. The atomic site
// mask remains race-safe if multiple audited writers touch one row; the exact value is retained as
// a corroborating match against B535's consumed word. This range is host-private and starts after
// every existing ordered-chain capture range.
namespace B1WriterProvenance {
constexpr u32 AlignDwords = 0x100;
constexpr u32 BaseDword =
    (B535InstructionTrace::BaseDword + B535InstructionTrace::DwordCount + AlignDwords - 1) &
    ~(AlignDwords - 1);
constexpr u32 HeaderDwords = 0x100;
constexpr u32 ScratchDword = BaseDword;
constexpr u32 MaxRows = 0x100000;
constexpr u32 SiteMaskBaseDword = BaseDword + HeaderDwords;
constexpr u32 TagBaseDword = SiteMaskBaseDword + MaxRows;
constexpr u32 ValueBaseDword = TagBaseDword + MaxRows;
constexpr u32 SourceBaseDword = ValueBaseDword + MaxRows;
constexpr u32 DwordCount = HeaderDwords + MaxRows * 4;
constexpr u32 Schema = 2;

// A compact exact trace for one stable cube row. The low 31 bits of its prepacked A3 v13 are
// invariant across launches, while the final B1 sign is known to change. Keeping this in the
// otherwise-unused provenance header avoids another large diagnostic allocation.
constexpr u32 FocusInputV13 = 0x18000038;
constexpr u32 FocusMagic = 0x41334c55; // "A3LU"
constexpr u32 FocusSchema = 1;
constexpr u32 FocusMaxChildren = 8;
constexpr u32 FocusMaxProbes = 8;
constexpr u32 FocusMagicDword = BaseDword + 1;
constexpr u32 FocusSchemaDword = BaseDword + 2;
constexpr u32 FocusInputDword = BaseDword + 3;
constexpr u32 FocusSeenMaskDword = BaseDword + 4;
constexpr u32 FocusHitMaskDword = BaseDword + 5;
constexpr u32 FocusProbeOverflowMaskDword = BaseDword + 6;
constexpr u32 FocusRowDword = BaseDword + 7;
constexpr u32 FocusMatchedPayloadBaseDword = BaseDword + 8;
constexpr u32 FocusInitialHashBaseDword = BaseDword + 16;
constexpr u32 FocusBucketCountBaseDword = BaseDword + 24;
constexpr u32 FocusProbeIndexBaseDword = BaseDword + 32;
constexpr u32 FocusLoadedKeyBaseDword =
    FocusProbeIndexBaseDword + FocusMaxChildren * FocusMaxProbes;
constexpr u32 FocusLoadedPayloadBaseDword =
    FocusLoadedKeyBaseDword + FocusMaxChildren * FocusMaxProbes;
constexpr u32 FocusTargetKeyBaseDword =
    FocusLoadedPayloadBaseDword + FocusMaxChildren * FocusMaxProbes;
constexpr u32 FocusInvocationCountDword = FocusTargetKeyBaseDword + FocusMaxChildren;
constexpr u32 FocusInvocationRowXorDword = FocusInvocationCountDword + 1;
constexpr u32 FocusInvocationRowSumDword = FocusInvocationRowXorDword + 1;
constexpr u32 FocusComparisonMaskDword = FocusInvocationRowSumDword + 1;
constexpr u32 FocusFinalCountDword = FocusComparisonMaskDword + 1;
constexpr u32 FocusFinalV17ZeroCountDword = FocusFinalCountDword + 1;
constexpr u32 FocusFinalForceCountDword = FocusFinalV17ZeroCountDword + 1;
constexpr u32 FocusFinalSelectedSignCountDword = FocusFinalForceCountDword + 1;
constexpr u32 FocusScratchBaseDword = FocusFinalSelectedSignCountDword + 1;
constexpr u32 FocusScratchDwords = 16;
constexpr u32 FocusEndDword = FocusScratchBaseDword + FocusScratchDwords;
static_assert(FocusEndDword <= BaseDword + HeaderDwords);

constexpr u32 Transform2020Bit = 1u << 0;
constexpr u32 Gather2278Bit = 1u << 1;
constexpr u32 Gather3c44Bit = 1u << 2;
constexpr u32 Seed144Bit = 1u << 3;
// Capture-only fields packed into bits unused by the four writer-site flags and A3 predicate flags.
constexpr u32 FocusProbeMaskShift = 8;
constexpr u32 FocusHitMaskShift = 8;
constexpr u32 FocusInvocationCountShift = 16;

enum Tag : u32 {
    Unwritten = 0,
    Seed144 = 1,
    Gather2278 = 2,
    Gather3c44 = 3,
    Transform2020 = 4,
};
} // namespace B1WriterProvenance
static_assert(B1WriterProvenance::BaseDword >=
              B535InstructionTrace::BaseDword + B535InstructionTrace::DwordCount);
static_assert(B1WriterProvenance::BaseDword + B1WriterProvenance::DwordCount <= 0xd00000);

// Opt-in, shader-side provenance for the two table publications in 63ddac84. The ring is wholly
// host-private and passive: guest EXEC only selects which lanes reserve a record, and no captured
// value feeds guest execution. Pair and count publications can be joined exactly by workgroup,
// lane, global invocation, and probe ordinal.
namespace A3LookupProducerTrace {
constexpr u32 AlignDwords = 0x100;
constexpr u32 BaseDword =
    (B1WriterProvenance::BaseDword + B1WriterProvenance::DwordCount + AlignDwords - 1) &
    ~(AlignDwords - 1);
constexpr u32 Magic = 0x50334c41; // "AL3P"
constexpr u32 Schema = 1;
constexpr u32 PairPc = 0x12c;
constexpr u32 CountPc = 0x1ac;
constexpr u32 ThreadsX = 64;

constexpr u32 HeaderDwords = 0x100;
constexpr u32 NextRecordDword = BaseDword;
constexpr u32 OverflowDword = BaseDword + 1;
constexpr u32 PairEventCountDword = BaseDword + 2;
constexpr u32 CountEventCountDword = BaseDword + 3;
constexpr u32 MagicDword = BaseDword + 4;
constexpr u32 SchemaDword = BaseDword + 5;
constexpr u32 ShaderHashDword = BaseDword + 6;
constexpr u32 PairPcDword = BaseDword + 7;
constexpr u32 CountPcDword = BaseDword + 8;
constexpr u32 RecordDwordsDword = BaseDword + 9;
constexpr u32 MaxRecordsDword = BaseDword + 10;
constexpr u32 RecordBaseOffsetDword = BaseDword + 11;
constexpr u32 ScratchBaseDword = BaseDword + 0x40;
constexpr u32 ScratchDwords = ThreadsX;

constexpr u32 RecordDwords = 16;
constexpr u32 MaxRecords = 0xf000;
constexpr u32 RecordBaseDword = BaseDword + HeaderDwords;
enum RecordField : u32 {
    Valid = 0,
    SitePc = 1,
    WorkgroupX = 2,
    WorkgroupY = 3,
    WorkgroupZ = 4,
    LocalInvocationX = 5,
    Lane = 6,
    GlobalInvocationX = 7,
    Ordinal = 8,
    PairSlot = 9,
    CountSlot = 10,
    Key = 11,
    Payload = 12,
    OldKey = 13,
    OldPayload = 14,
    SiteFlags = 15,
};
constexpr u32 RecordValid = 0xa363dd01;
constexpr u32 PairSiteFlag = 1u << 0;
constexpr u32 CountSiteFlag = 1u << 1;
constexpr u32 MissingValue = 0xffffffff;
constexpr u32 DwordCount = HeaderDwords + MaxRecords * RecordDwords;

constexpr u32 RecordDword(u32 record, u32 field) {
    return RecordBaseDword + record * RecordDwords + field;
}
} // namespace A3LookupProducerTrace
static_assert(A3LookupProducerTrace::ScratchBaseDword +
                  A3LookupProducerTrace::ScratchDwords <=
              A3LookupProducerTrace::RecordBaseDword);
static_assert(A3LookupProducerTrace::BaseDword >=
              B1WriterProvenance::BaseDword + B1WriterProvenance::DwordCount);
static_assert(A3LookupProducerTrace::BaseDword + A3LookupProducerTrace::DwordCount <= 0xd00000);

constexpr u32 OrderedClearOffsetBytes = TraversalCompletionIndex * sizeof(u32);
constexpr u32 OrderedClearSizeBytes = sizeof(u32);

// Host-private GDS counters used by the opt-in GatherVoxels stage trace. The 8 MiB GDS backing
// contains 0x200000 dwords; all ordered-count scratch and metadata end below this high range.
constexpr u32 GatherStageTraceBaseDword = 0x1e0000;
constexpr u32 GatherStageTraceCount = 71;
// Keep numeric samples separate from the aggregate counters so a sampled register value cannot
// be mistaken for a stage count. Most samples come from workgroup zero; the tail records the
// first eight workgroups' candidate-buffer values and a validity mask.
constexpr u32 GatherNumericTraceBaseDword = GatherStageTraceBaseDword + 0x100;
constexpr u32 GatherFocusKeyBase = 0x100001c0;
constexpr u32 GatherFocusKeyCount = 8;
enum GatherFocusTraceSlot : u32 {
    GatherFocusInput = 46,
    GatherFocusLookupEligible = 47,
    GatherFocusDecisionReached = 48,
    GatherFocusProbeNonzero = 49,
    GatherFocusProbeZero = 50,
    GatherFocusWaveDecisionReached = 51,
    GatherFocusWaveRejected = 52,
    GatherFocusWaveAccepted = 53,
    GatherFocusRejectedStore = 54,
    GatherFocusAcceptedLane = 55,
    GatherFocusAcceptedAtomic = 56,
    GatherFocusAcceptedRowStore = 57,
    GatherFocusFinalStore = 58,
};
constexpr u32 GatherNumericTraceCount = GatherFocusFinalStore + 1;
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
// Host-private storage for an opt-in capture of the complete VS 3706083c -> FS ce3b8413
// interface. Values are raw bits in [instance][vertex][attribute][component] order, with the
// attributes Position0, Param0, Param1, and Param2. RenderTargetIndex is kept in a parallel scalar
// array. One validity dword per instance/vertex uses bits 0-15 for the vec4s and bit 16 for the
// layer. The final dword is both the arm flag and instance limit.
// The observed sculpt draws contain 175-243 instances; reject larger draws instead of silently
// truncating a fixture. This range remains below the end of the 8 MiB GDS allocation.
constexpr u32 Vs370InterfaceCaptureBaseDword = 0x1e3000;
constexpr u32 Vs370InterfaceCaptureMaxInstances = 512;
constexpr u32 Vs370InterfaceCaptureVertices = 8;
constexpr u32 Vs370InterfaceCaptureAttributes = 4;
constexpr u32 Vs370InterfaceCaptureComponents = 4;
constexpr u32 Vs370InterfaceCaptureValuesPerVertex =
    Vs370InterfaceCaptureAttributes * Vs370InterfaceCaptureComponents;
constexpr u32 Vs370InterfaceCaptureValuesPerInstance =
    Vs370InterfaceCaptureVertices * Vs370InterfaceCaptureValuesPerVertex;
constexpr u32 Vs370InterfaceCaptureValueCount =
    Vs370InterfaceCaptureMaxInstances * Vs370InterfaceCaptureValuesPerInstance;
constexpr u32 Vs370InterfaceCaptureLayerBaseDword =
    Vs370InterfaceCaptureBaseDword + Vs370InterfaceCaptureValueCount;
constexpr u32 Vs370InterfaceCaptureLayerCount =
    Vs370InterfaceCaptureMaxInstances * Vs370InterfaceCaptureVertices;
constexpr u32 Vs370InterfaceCaptureValidityBaseDword =
    Vs370InterfaceCaptureLayerBaseDword + Vs370InterfaceCaptureLayerCount;
constexpr u32 Vs370InterfaceCaptureValidityCount =
    Vs370InterfaceCaptureMaxInstances * Vs370InterfaceCaptureVertices;
constexpr u32 Vs370InterfaceCaptureArmDword =
    Vs370InterfaceCaptureValidityBaseDword + Vs370InterfaceCaptureValidityCount;
constexpr u32 Vs370InterfaceCaptureDwordCount = Vs370InterfaceCaptureValueCount +
                                                Vs370InterfaceCaptureLayerCount +
                                                Vs370InterfaceCaptureValidityCount + 1;
// Bounded atlas-sample trace for the ce3b8413 sculpt ray marcher. The first Param1.x reaching the
// initial sample claims one sculpt record. For that record, the first dynamic invocation at each
// static sample site is stored independently. These rows are intended for direct sampled-value
// versus atlas-byte checks; they are not asserted to come from the same fragment.
namespace Ce3FleckTrace {
constexpr u32 BaseDword =
    (Vs370InterfaceCaptureBaseDword + Vs370InterfaceCaptureDwordCount + 0xffU) & ~0xffU;
constexpr u32 HeaderDwords = 10;
constexpr u32 ArmDword = BaseDword;
constexpr u32 ClaimedParam1Dword = BaseDword + 1;
constexpr u32 SeenDword = BaseDword + 2;
constexpr u32 DiscardConditionDword = BaseDword + 3;
constexpr u32 ConditionalDiscardInvocationsDword = BaseDword + 4;
constexpr u32 ConditionalDiscardTrueDword = BaseDword + 5;
constexpr u32 ConditionalDiscardFalseDword = BaseDword + 6;
constexpr u32 UnconditionalDiscardInvocationsDword = BaseDword + 7;
constexpr u32 InitialSampleInvocationsDword = BaseDword + 8;
constexpr u32 RecordBaseDword = BaseDword + HeaderDwords;
constexpr u32 StaticSampleSites = 2;
constexpr u32 RecordDwords = 15;
constexpr u32 FragCoord = 0;
constexpr u32 Param0 = 4;
constexpr u32 Coordinates = 8;
constexpr u32 Sample = 11;
constexpr u32 ClaimSentinel = 0xffffffffU;
constexpr u32 DwordCount = HeaderDwords + StaticSampleSites * RecordDwords;

constexpr u32 RecordDword(u32 site, u32 field) {
    return RecordBaseDword + site * RecordDwords + field;
}

} // namespace Ce3FleckTrace
// Separate whole-draw coverage and decision inventory for ce3b8413. Param1.y is observed as a flat
// per-sculpt/object label in the captured edit-mode draw; Param1.x atlas records are reused across
// those objects and therefore are secondary min/max metadata rather than the hash key. Keeping this
// mode independent from the coordinate/sample trace avoids candidate-dependent capture branches
// before the guest's final ballot. Each event uses a bounded branchless probe sequence; any
// unresolved collision invalidates completeness rather than being attributed to the resident key.
// Keep the table in the unused tail of the 52 MiB private-GDS allocation so a complete edit-mode
// sculpt draw does not crowd the older 8 MiB diagnostic ranges.
namespace Ce3CoverageTrace {
constexpr u32 BaseDword =
    (A3LookupProducerTrace::BaseDword + A3LookupProducerTrace::DwordCount + 0xffU) & ~0xffU;
constexpr u32 ArmDword = BaseDword;
// The proof-grade edit-mode draw observed eight Param1.y labels; the scene is independently known
// to contain eleven stamped slots. Leave ample headroom for other draws; runtime miss counters
// remain authoritative and make every unresolved collision explicit.
constexpr u32 Slots = 256;
constexpr u32 SlotBits = 8;
constexpr u32 Probes = 8;
constexpr u32 KeysBaseDword = BaseDword + 1;
constexpr u32 Param1XMinBaseDword = KeysBaseDword + Slots;
constexpr u32 Param1XMaxBaseDword = Param1XMinBaseDword + Slots;
constexpr u32 FragXMinBaseDword = Param1XMaxBaseDword + Slots;
constexpr u32 FragXMaxBaseDword = FragXMinBaseDword + Slots;
constexpr u32 FragYMinBaseDword = FragXMaxBaseDword + Slots;
constexpr u32 FragYMaxBaseDword = FragYMinBaseDword + Slots;
constexpr u32 InitialSampleBaseDword = FragYMaxBaseDword + Slots;
// Each packed sample-bin word holds two independent 16-bit counters. The low half counts atlas
// samples below 0.5 (negative after ce3's `4 * sample - 2` decode); the high half counts samples
// at or above 0.5. The overflow counter invalidates a capture before either half can wrap.
constexpr u32 InitialSampleBinsBaseDword = InitialSampleBaseDword + Slots;
constexpr u32 RaymarchSampleBinsBaseDword = InitialSampleBinsBaseDword + Slots;
constexpr u32 ConditionalTrueBaseDword = RaymarchSampleBinsBaseDword + Slots;
constexpr u32 ConditionalFalseBaseDword = ConditionalTrueBaseDword + Slots;
constexpr u32 UnconditionalBaseDword = ConditionalFalseBaseDword + Slots;

// V23 is the fully decoded procedural-plus-atlas decision value at guest PCs 0x338 (initial) and
// 0x430 (raymarch loop). The four exhaustive classes use +/-0.001, the same refinement epsilon the
// guest uses at PC 0x454, so values on either boundary remain in the near-zero class.
constexpr u32 DecisionSites = 2;
constexpr u32 DecisionCategories = 4;
constexpr u32 DecisionNaN = 0;
constexpr u32 DecisionRobustNegative = 1;
constexpr u32 DecisionNearZero = 2;
constexpr u32 DecisionRobustPositive = 3;
constexpr float DecisionEpsilon = 0.001f;
constexpr u32 DecisionBaseDword = UnconditionalBaseDword + Slots;
constexpr u32 DecisionGuestNegativeBaseDword =
    DecisionBaseDword + DecisionSites * DecisionCategories * Slots;
constexpr u32 DecisionGuestNonnegativeBaseDword =
    DecisionGuestNegativeBaseDword + DecisionSites * Slots;
constexpr u32 LoopHitBaseDword = DecisionGuestNonnegativeBaseDword + DecisionSites * Slots;
constexpr u32 LoopBoundNoHitBaseDword = LoopHitBaseDword + Slots;

// Guest trinary instructions at PCs 0x28c, 0x2c8, 0x374, and 0x3ec. For each site retain an
// invocation denominator and independent NaN counts for three post-modifier inputs plus output.
constexpr u32 TrinarySites = 4;
constexpr u32 TrinaryValues = 4;
constexpr u32 TrinaryInvocationBaseDword = LoopBoundNoHitBaseDword + Slots;
constexpr u32 TrinaryNaNBaseDword = TrinaryInvocationBaseDword + TrinarySites * Slots;

constexpr u32 MissInitialDword =
    TrinaryNaNBaseDword + TrinarySites * TrinaryValues * Slots;
constexpr u32 MissRaymarchSampleDword = MissInitialDword + 1;
constexpr u32 MissConditionalTrueDword = MissRaymarchSampleDword + 1;
constexpr u32 MissConditionalFalseDword = MissRaymarchSampleDword + 2;
constexpr u32 MissUnconditionalDword = MissRaymarchSampleDword + 3;
constexpr u32 MissDecisionBaseDword = MissRaymarchSampleDword + 4;
constexpr u32 MissLoopDword = MissDecisionBaseDword + DecisionSites;
constexpr u32 MissTrinaryBaseDword = MissLoopDword + 1;
constexpr u32 SampleBinOverflowDword = MissTrinaryBaseDword + TrinarySites;
constexpr u32 CounterOverflowDword = SampleBinOverflowDword + 1;
// Rejected bounded-probe events target this unused sink so metadata updates stay branchless before
// the guest shader's ballot. No sink value is consumed or reported.
constexpr u32 SinkDword = CounterOverflowDword + 1;
constexpr u32 DwordCount = SinkDword - BaseDword + 1;
constexpr u32 ClaimSentinel = 0xffffffffU;
constexpr u32 PackedBinMask = 0xffffU;
constexpr u32 PackedBinHighIncrement = 1U << 16;

constexpr u32 DecisionDword(u32 site, u32 category, u32 slot = 0) {
    return DecisionBaseDword + (site * DecisionCategories + category) * Slots + slot;
}

constexpr u32 TrinaryInvocationDword(u32 site, u32 slot = 0) {
    return TrinaryInvocationBaseDword + site * Slots + slot;
}

constexpr u32 TrinaryNaNDword(u32 site, u32 value, u32 slot = 0) {
    return TrinaryNaNBaseDword + (site * TrinaryValues + value) * Slots + slot;
}

static_assert(Slots == (1U << SlotBits));
static_assert(Probes > 0 && Probes <= Slots);
static_assert(DecisionDword(DecisionSites - 1, DecisionCategories - 1, Slots - 1) + 1 ==
              DecisionGuestNegativeBaseDword);
static_assert(TrinaryInvocationDword(TrinarySites - 1, Slots - 1) + 1 ==
              TrinaryNaNBaseDword);
static_assert(TrinaryNaNDword(TrinarySites - 1, TrinaryValues - 1, Slots - 1) + 1 ==
              MissInitialDword);
} // namespace Ce3CoverageTrace
// Host-private storage for a diagnostic capture of the four dynamic ReadConst results consumed by
// ce3b8413. The shader writes exactly what its BDA path returned; the host never reads or registers
// the guest constant page before the draw. Keep this range separate from the VS370 fixture so the
// two diagnostics can never influence one another.
constexpr u32 Ce3ReadConstCaptureBaseDword = 0x1fff00;
constexpr u32 Ce3ReadConstCaptureArmDword = Ce3ReadConstCaptureBaseDword;
constexpr u32 Ce3ReadConstCaptureSeenDword = Ce3ReadConstCaptureBaseDword + 1;
constexpr u32 Ce3ReadConstCaptureTupleBaseDword = Ce3ReadConstCaptureBaseDword + 2;
constexpr u32 Ce3ReadConstCaptureTupleDwords = 4;
constexpr u32 Ce3ReadConstCaptureDynamicReads = 4;
constexpr u32 Ce3ReadConstCaptureDwordCount =
    2 + Ce3ReadConstCaptureDynamicReads * Ce3ReadConstCaptureTupleDwords;
// Host-private storage for an opt-in diagnostic of the 3D ImageGather fallback. The first compute
// invocation that actually reaches each static site atomically claims its record, then stores every
// source coordinate, complete fetched texel, selected component, and final output component. This
// sits immediately below the ce3 ReadConst range and does not overlap guest-visible storage.
namespace ImageGather3DCapture {
constexpr u32 BaseDword = 0x1ff000;
constexpr u32 ArmDword = BaseDword;
constexpr u32 TargetHashLoDword = BaseDword + 1;
constexpr u32 TargetHashHiDword = BaseDword + 2;
constexpr u32 ClaimDword = BaseDword + 3;
constexpr u32 SeenDword = BaseDword + 4;
constexpr u32 RecordBaseDword = BaseDword + 5;
constexpr u32 MaxSites = 8;
constexpr u32 RecordDwords = 56;
constexpr u32 ShaderHashLo = 0;
constexpr u32 ShaderHashHi = 1;
constexpr u32 Stage = 2;
constexpr u32 Site = 3;
constexpr u32 Workgroup = 4;
constexpr u32 LocalInvocation = 7;
constexpr u32 Component = 10;
constexpr u32 ZFilter = 11;
constexpr u32 NormalizedCoordinates = 12;
constexpr u32 Dimensions = 15;
constexpr u32 SourceCoordinates = 18;
constexpr u32 SourceTexels = 30;
constexpr u32 SelectedComponents = 46;
constexpr u32 EmittedComponents = 50;
// Low 16 bits of the image/sampler handle used by the gather. This is the flattened image binding
// and therefore indexes the exact host image/view bound for this dispatch.
constexpr u32 ImageBinding = 54;
constexpr u32 DwordCount = 5 + MaxSites * RecordDwords;

constexpr u32 RecordDword(u32 site, u32 field) {
    return RecordBaseDword + site * RecordDwords + field;
}
} // namespace ImageGather3DCapture
// Exact post-consumer boundary capture for compute shaders that use the 3D-gather fallback. Each
// static StoreBuffer site keeps the first few complete outgoing records, plus an immediate reload
// from the destination buffer. This distinguishes malformed shader results from corruption in the
// buffer write or a later consumer without changing any value used by the guest shader.
namespace ImageGather3DConsumerCapture {
constexpr u32 BaseDword = 0x1f6000;
constexpr u32 SeenDword = BaseDword;
constexpr u32 CounterBaseDword = BaseDword + 1;
constexpr u32 MaxSites = 8;
constexpr u32 MaxRecordsPerSite = 16;
constexpr u32 RecordDwords = 24;
constexpr u32 RecordBaseDword = CounterBaseDword + MaxSites;
constexpr u32 ShaderHashLo = 0;
constexpr u32 ShaderHashHi = 1;
constexpr u32 Site = 2;
constexpr u32 Workgroup = 3;
constexpr u32 LocalInvocation = 6;
constexpr u32 Handle = 9;
constexpr u32 Components = 10;
constexpr u32 RawAddress = 11;
constexpr u32 ResolvedAddress = 12;
constexpr u32 InputValues = 13;
constexpr u32 ReloadedValues = 17;
constexpr u32 MatchMask = 21;
constexpr u32 BufferType = 22;
constexpr u32 DwordCount = 1 + MaxSites + MaxSites * MaxRecordsPerSite * RecordDwords;

constexpr u32 CounterDword(u32 site) {
    return CounterBaseDword + site;
}

constexpr u32 RecordDword(u32 site, u32 record, u32 field) {
    return RecordBaseDword +
           (site * MaxRecordsPerSite + record) * RecordDwords + field;
}
} // namespace ImageGather3DConsumerCapture
static_assert(GatherStageTraceBaseDword >= OrderedScratchBaseDword + OrderedScratchDwords);
static_assert(GatherStageTraceBaseDword + GatherStageTraceCount <= 0x200000);
static_assert(GatherStageTraceBaseDword + GatherStageTraceCount <= GatherNumericTraceBaseDword);
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
static_assert(ModelProducerNumericBaseDword + ModelProducerNumericCount <=
              Vs370InterfaceCaptureBaseDword);
static_assert(Vs370InterfaceCaptureBaseDword + Vs370InterfaceCaptureDwordCount <= 0x200000);
static_assert(Vs370InterfaceCaptureBaseDword + Vs370InterfaceCaptureDwordCount <=
              Ce3FleckTrace::BaseDword);
static_assert(Ce3FleckTrace::BaseDword + Ce3FleckTrace::DwordCount <=
              ImageGather3DConsumerCapture::BaseDword);
static_assert(ImageGather3DConsumerCapture::BaseDword + ImageGather3DConsumerCapture::DwordCount <=
              ImageGather3DCapture::BaseDword);
static_assert(ImageGather3DCapture::BaseDword + ImageGather3DCapture::DwordCount <=
              Ce3ReadConstCaptureBaseDword);
static_assert(Ce3ReadConstCaptureBaseDword + Ce3ReadConstCaptureDwordCount <= 0x200000);
static_assert(Ce3CoverageTrace::BaseDword >=
              A3LookupProducerTrace::BaseDword + A3LookupProducerTrace::DwordCount);
static_assert(Ce3CoverageTrace::BaseDword + Ce3CoverageTrace::DwordCount <= 0xd00000);

inline bool CaptureVs370Interface() {
    static const bool enabled = [] {
        const char* value = std::getenv("SHADPS4_DREAMS_VS370_INTERFACE_CAPTURE");
        const char* ordered_chain = std::getenv("SHADPS4_DREAMS_ORDERED_CHAIN_CAPTURE_DIR");
        return (value != nullptr && value[0] == '1' && value[1] == '\0') ||
               (ordered_chain != nullptr && ordered_chain[0] != '\0');
    }();
    return enabled;
}

// One-shot host lineage capture for the sculpt visibility pipeline.  This deliberately remains
// separate from CaptureOrderedChain(): the latter also instruments QueueProducer, several B1
// writers, and VS370.  The upstream lineage capture needs only B535's fixed-slot membership
// records; d8 and 7aa are captured from their exact bound Vulkan buffers by the rasterizer.
inline bool CaptureUpstreamLineage() {
    static const bool enabled = [] {
        const char* value = std::getenv("SHADPS4_DREAMS_UPSTREAM_CHAIN_CAPTURE_DIR");
        return value != nullptr && value[0] != '\0';
    }();
    return enabled;
}

inline bool CaptureCe3ReadConst() {
    static const bool enabled = [] {
        const char* value = std::getenv("SHADPS4_DREAMS_CE3_READCONST_CAPTURE");
        return value != nullptr && value[0] == '1' && value[1] == '\0';
    }();
    return enabled;
}

inline bool CaptureCe3FleckTrace() {
    static const bool enabled = [] {
        const char* value = std::getenv("SHADPS4_DREAMS_CE3_FLECK_CAPTURE_DIR");
        return value != nullptr && value[0] != '\0';
    }();
    return enabled;
}

inline bool CaptureCe3CoverageTrace() {
    static const bool enabled = [] {
        const char* value = std::getenv("SHADPS4_DREAMS_CE3_COVERAGE_CAPTURE_DIR");
        return value != nullptr && value[0] != '\0';
    }();
    return enabled;
}

inline bool CaptureImageGather3D() {
    static const bool enabled = [] {
        const char* value = std::getenv("SHADPS4_DREAMS_3D_GATHER_CAPTURE");
        const char* cross_check = std::getenv("SHADPS4_DREAMS_84AA_GATHER_CROSSCHECK");
        return (value != nullptr && value[0] == '1' && value[1] == '\0') ||
               (cross_check != nullptr && cross_check[0] == '1' && cross_check[1] == '\0');
    }();
    return enabled;
}

inline bool CaptureB535Membership() {
    static const bool enabled = [] {
        const char* value = std::getenv("SHADPS4_DREAMS_B535_MEMBERSHIP_CAPTURE");
        const char* instruction_trace =
            std::getenv("SHADPS4_DREAMS_B535_INSTRUCTION_TRACE");
        const char* ordered_chain = std::getenv("SHADPS4_DREAMS_ORDERED_CHAIN_CAPTURE_DIR");
        return (value != nullptr && value[0] == '1' && value[1] == '\0') ||
               (instruction_trace != nullptr && instruction_trace[0] == '1' &&
                instruction_trace[1] == '\0') ||
               (ordered_chain != nullptr && ordered_chain[0] != '\0') ||
               CaptureUpstreamLineage();
    }();
    return enabled;
}

inline bool CaptureB535InstructionTrace() {
    static const bool enabled = [] {
        const char* value = std::getenv("SHADPS4_DREAMS_B535_INSTRUCTION_TRACE");
        return value != nullptr && value[0] == '1' && value[1] == '\0';
    }();
    return enabled;
}

inline bool ForceB535FirstGateFailure() {
    static const bool enabled = [] {
        const char* value = std::getenv("SHADPS4_DREAMS_B535_FORCE_FIRST_GATE_FAILURE");
        return value != nullptr && value[0] == '1' && value[1] == '\0';
    }();
    return enabled;
}

inline bool CaptureOrderedChain() {
    static const bool enabled = [] {
        const char* ordered_chain = std::getenv("SHADPS4_DREAMS_ORDERED_CHAIN_CAPTURE_DIR");
        return ordered_chain != nullptr && ordered_chain[0] != '\0';
    }();
    return enabled;
}

inline bool CaptureB1WriterProvenance() {
    return CaptureOrderedChain();
}

inline bool CaptureA3LookupProducerTrace() {
    static const bool enabled = [] {
        const char* value = std::getenv("SHADPS4_DREAMS_A3_PRODUCER_TRACE");
        return value != nullptr && value[0] == '1' && value[1] == '\0';
    }();
    return enabled;
}

inline bool CaptureGatherStageTrace() {
    static const bool enabled = [] {
        const char* value = std::getenv("SHADPS4_DREAMS_GATHER_STAGE_TRACE");
        return value != nullptr && value[0] == '1' && value[1] == '\0';
    }();
    return enabled;
}

inline bool CaptureGatherFocusTrace() {
    static const bool enabled = [] {
        const char* value = std::getenv("SHADPS4_DREAMS_GATHER_FOCUS_TRACE");
        return value != nullptr && value[0] == '1' && value[1] == '\0';
    }();
    return enabled;
}

constexpr bool IsB1WriterProvenanceShader(u64 hash) {
    return hash == B1SeedWriterShader || hash == GatherVoxelsShader ||
           hash == B1TransformWriterShader;
}

inline bool IsCaptureInstrumentedShader(u64 hash) {
    return (CaptureVs370Interface() && hash == Vs370InterfaceCaptureShader) ||
           (CaptureCe3ReadConst() && hash == Ce3ReadConstCaptureShader) ||
           (CaptureCe3FleckTrace() && hash == Ce3ReadConstCaptureShader) ||
           (CaptureCe3CoverageTrace() && hash == Ce3ReadConstCaptureShader) ||
           (CaptureB535Membership() && hash == TraversalShader) ||
           (CaptureOrderedChain() && hash == QueueProducerShader) ||
           (CaptureB1WriterProvenance() && IsB1WriterProvenanceShader(hash)) ||
           (CaptureA3LookupProducerTrace() && hash == A3LookupProducerShader) ||
           ((CaptureGatherStageTrace() || CaptureGatherFocusTrace()) &&
            hash == GatherVoxelsShader) ||
           CaptureImageGather3D();
}

constexpr bool IsSpriteCullShader(u64 hash) {
    return hash == SpriteCullShader || hash == SpriteCullShaderAlt ||
           hash == SpriteCullShaderCusa04301;
}

constexpr bool NeedsBranchlessOrderedCountAdd(u64 hash) {
    return hash == TraversalShader || hash == QueueProducerShader ||
           hash == QueueProducerShaderAlt || hash == IndirectArgsShader ||
           hash == VisibilityCandidateCompactShader || hash == GatherInputCompactShader ||
           hash == VisibilityListCompactShader || hash == VisibilityListCompactShaderAlt ||
           hash == SceneCompactShader;
}

constexpr bool UsesAlwaysExactOrderedCountReplay(u64 hash) {
    return hash == TraversalShader || hash == QueueProducerShaderAlt ||
           hash == B1SeedWriterShader || hash == SculptVolumeWriterShader ||
           hash == SculptSurfaceCompactShader ||
           hash == SculptSurfaceFinalizeShader || hash == IndirectArgsShader ||
           hash == VisibilityCandidateCompactShader || hash == GatherInputCompactShader ||
           hash == VisibilityListCompactShader || hash == VisibilityListCompactShaderAlt;
}

inline bool UseSceneCompactExactOrderedCountReplay() {
    const char* value = std::getenv("SHADPS4_DREAMS_SCENE_COMPACT_EXACT_DOC");
    return value != nullptr && value[0] == '1' && value[1] == '\0';
}

inline bool UsesExactOrderedCountReplay(u64 hash) {
    return UsesAlwaysExactOrderedCountReplay(hash) ||
           (hash == SceneCompactShader && UseSceneCompactExactOrderedCountReplay());
}
static_assert(NeedsBranchlessOrderedCountAdd(QueueProducerShader));
static_assert(!UsesAlwaysExactOrderedCountReplay(QueueProducerShader));

inline bool UseSculptOrderedCountLeaderBarrier(u64 hash) {
    if (hash != SculptVolumeWriterShader) {
        return false;
    }
    const char* value = std::getenv("SHADPS4_DREAMS_SCULPT_LEADER_BARRIER");
    return value != nullptr && value[0] == '1' && value[1] == '\0';
}

constexpr bool NeedsLdsMemoryBarrier(u64 hash) {
    return hash == TemporalResolveShader;
}

constexpr bool NeedsPostFillLdsControlBarrier(u64 hash) {
    return hash == SpatialReconstructionPrepareShader || hash == SpatialReconstructionShader;
}

constexpr bool NeedsGatherVoxelsLdsPhaseBarriers(u64 hash) {
    return hash == GatherVoxelsShader;
}

constexpr bool NeedsDreamsLdsBarrier(u64 hash) {
    return NeedsLdsMemoryBarrier(hash) || NeedsPostFillLdsControlBarrier(hash) ||
           NeedsGatherVoxelsLdsPhaseBarriers(hash);
}

} // namespace Shader::DreamsCompat
