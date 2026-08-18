# Reproduction Notes

## Setup

- Windows
- Local Release build of shadPS4 with the August 17 3D-rendering patch
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
4. Enter the tutorial, creation content, or an offline collection such as `Mm Characters`.
5. Confirm that UI and 3D content cover the full screen and that the FPS overlay reaches 30 FPS.
6. Preserve the log immediately after the failing scene because the next launch overwrites it.

## Expected current result

- Full-screen 3D menu content, flecks, UI, and the imp render correctly.
- The validated `Mm Characters` screen runs at the 30 FPS cap with diagnostics disabled.
- A black screen, top-edge render sliver, or 1 FPS menu is a regression or a diagnostic-mode result.
- Broad playability is still unverified; record the exact scene and action for any remaining fault.

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
- Indirect-argument shader `0x90272fc4` command counts and undefined IR values
- Traversal `0xb535c6c8` indirect dimensions
- GDS input base/total before traversal
- Unexpected `scheduler.Finish()` calls before the intro or presentation
- AV decode timestamps compared with host presentation timestamps
- Compute `RELEASE_MEM` destination, data selection, GDS range, and pipe interrupt

## Patch base

Apply `patches/dreams-focused-20260817-3d-rendering.patch` to upstream commit
`555c458c9fdd33cb4686492374519c7bb112a891`.

The patch is an investigation snapshot, not a minimal upstream-ready series.
