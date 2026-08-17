# Reproduction Notes

## Setup

- Windows
- Local Release build of shadPS4 with the August 17 cumulative patch
- Game serial `CUSA04301`
- Per-game GPU readback mode `2`
- Pipeline cache disabled while iterating on shader behavior
- Offline operation is acceptable and preferred for the current target

Use legal game and firmware files. This repository does not include game data, firmware, keys, or
save data.

## Current manual path

1. Launch Dreams using the patched shadPS4 build.
2. Let the intro/startup path proceed; do not use automated controller input.
3. Pass Continue and Preferences if they appear.
4. Enter the tutorial or use an already progressed personal save.
5. Observe whether the scene remains black, whether a small top-left fragment appears, and whether
   the imp spawns.
6. Preserve the log immediately after the failing scene because the next launch overwrites it.

## Expected current symptoms

- Intro timing is approximately correct, but presentation is extremely slow.
- Intro video may be invisible while audio advances at roughly 1-2 FPS.
- Startup screens have rendered in older builds, so a permanently black startup screen is a
  regression rather than the desired baseline.
- Tutorial logic can continue and the imp can respond even while scene geometry is black.
- The low-FPS Preferences warning may recur without the Dreams-specific guard.

## Diagnostic modes

The working source contains targeted environment-controlled diagnostics:

- `SHADPS4_DREAMS_DEP_TRACE=1`: buffer dependency tracing
- `SHADPS4_DREAMS_ORDERED_COUNTERS=1`: ordered-counter/producer inspection
- `SHADPS4_DREAMS_CSG_TRACE=1`: CSG replay state
- `SHADPS4_DREAMS_DIAGNOSTICS=1`: broad Dreams diagnostics

The broad and ordered-counter modes perform GPU waits and can themselves cause severe slowdown.
They are evidence-gathering modes only and must not be used for performance validation.

## High-value log checks

- Producer `0x2bfebd3c` record count and SRT address
- Contents and modification state of producer buffers
- Traversal `0xb535c6c8` indirect dimensions
- GDS input base/total before traversal
- Unexpected `scheduler.Finish()` calls before the intro or presentation
- AV decode timestamps compared with host presentation timestamps
- Compute `RELEASE_MEM` destination, data selection, GDS range, and pipe interrupt

## Patch base

Apply `patches/dreams-focused-20260817-current.patch` to upstream commit
`555c458c9fdd33cb4686492374519c7bb112a891`.

The patch is an investigation snapshot, not a minimal upstream-ready series.
