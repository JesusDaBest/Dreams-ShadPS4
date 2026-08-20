# Reproduction Notes

## Setup

- Windows
- Legal `CUSA04301` game and firmware files
- Release build made from the August 19 experimental source
- Per-game readback mode used in the latest test: `1`
- Present mode used in the latest test: `Immediate`
- Pipeline cache disabled while shader behavior is changing
- Offline environment used in the latest test: `SHADPS4_DREAMS_FAKE_PSN=offline`

These settings describe the captured run; they are not all proven requirements.

Use a dedicated build directory with a local portable `user` folder. Do not point this experimental
build at another shadPS4 installation's saves or caches.

## Stable-baseline test

1. Launch Dreams without broad diagnostic environment variables.
2. Pass Continue, consent/EULA, and Preferences if they appear.
3. Enter DreamShaping, open a fixed saved scene, and enter edit mode.
4. Record whether the imp, grid, UI, gadgets, tweak panels, sculpt preview, placed sculpt, and paint
   stroke are visible.
5. Rotate the camera without editing the scene and check whether geometry changes position.
6. Preserve the game log immediately; the next launch can replace it.

Expected known result is incomplete: UI/imp/grid/gadgets can render, while sculpts and paint remain
missing and tweak panels may be black.

## Dispatch-base candidate test

The August 19 candidate should create ordered-count compute pipelines with the Vulkan dispatch-base
flag. In the known scene, this changed invisible geometry into oversized grey shapes. Correct output
must not be assumed merely because more pixels appear.

Reject the candidate if any of these occur:

- tweak panels turn black;
- geometry shifts when only the camera moves;
- a sculpt is audible but invisible or unselectable;
- paint/fleck strokes are absent;
- the imp, grid, UI, or gadgets regress;
- performance is measured while forced-wait diagnostics are active.

## Save-space test

1. Open Dreams' Limits Info page.
2. Record local usage, maximum blocks, creations, versions, and photos.
3. Attempt to create and save a small scene.
4. Confirm that host free disk space is not confused with the game's 1 GiB save quota.

The corrected emulator counts each regular file in rounded 32 KiB PS4 blocks and clamps free blocks
at zero. It must not report an unsigned-underflow value such as the observed false `4 GB used` state.

## Targeted diagnostics

The source includes environment-gated and trigger-file diagnostics for:

- compute and buffer dependency tracing;
- ordered counters and GDS state;
- producer records and input buffers;
- indirect geometry command scanning;
- geometry input dumps for shader `0xd25db925`;
- one-shot GPU draw/dispatch timing;
- RenderDoc capture support;
- CPU-write watches and crash context.

Environment-variable names and trigger handling are searchable under
`src/video_core/renderer_vulkan/vk_rasterizer.cpp`. Enable only one focused diagnostic at a time.
Many paths call `scheduler.Finish()` and can reduce Dreams to about 1 FPS.

## High-value checks

- traversal shader `0xb535c6c8` M0 value and logical wave ID;
- all four `DS_ORDERED_COUNT` return values for one guest wave;
- GDS ordered-counter values before and after release/done;
- queue producer `0x2bfebd3c` record count;
- scene-compaction shader `0x3937a849` output;
- indirect-argument shader `0x90272fc4` command contents, not only nonzero count;
- geometry draw VS `0xd25db925` and FS `0x3f6e1a00` input ranges;
- whether camera-only changes alter generated position records;
- unexpected full GPU waits during a non-diagnostic run.

## Patch

Apply `patches/dreams-focused-20260819-experimental.patch` to upstream commit
`555c458c9fdd33cb4686492374519c7bb112a891`.

Verify SHA-256:

```text
CCAE708A596A9F40833E027A12723C5698452CA8596FD0E1DE1D1F9F4E1B5E60
```

The patch is cumulative, experimental, and not upstream-ready.
