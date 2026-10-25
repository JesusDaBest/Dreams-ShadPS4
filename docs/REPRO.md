# Reproduction Notes

For applying and compiling the retained source fixes, start with [SOURCE_BUILD.md](SOURCE_BUILD.md). The working local profile uses **Precise readbacks**. The active game metadata reports CUSA04301 **VERSION 02.64** and **APP_VER 01.00**; APP_VER alone must not be treated as the content update version. Game update 2.65 remains unverified.

> **Latest September 4 runtime test:** [CLEAN_CORE_TEST_20260904.md](CLEAN_CORE_TEST_20260904.md) records two actual official-core runs with the general GPU corrections. Both fail translating `DS_ORDERED_COUNT` in `cbac06d2`; Dreams is not working on this build. This supersedes the earlier plan to establish a clean-core run. Earlier GPU provenance and correction tests remain in [HANDOFF_20260904.md](HANDOFF_20260904.md).

## September 2 focused producer-count test

Do not use the September 2 WIP executable as a baseline performance build. For the next diagnostic
run, start from a diagnostic-safe build with a warm shader cache and enable only:

```text
SHADPS4_DREAMS_PRODUCER_COUNT_READBACK=1
```

Enter the fixed scene and trigger the held/edit state once. Preserve the log before another launch.
The result is meaningful only if it records flattened SRT word 41 before and after refresh, word 18,
the producer's final count, and indirect X:

- word 41 changes zero to nonzero: stale flattened-SRT / GPU-to-CPU coherency candidate;
- word 41 remains zero: trace its writer/publication edge;
- word 41 is nonzero while the producer publishes zero: capture its predicate and ordered payload.

Do not combine this with broad ordered-counter, image, or atlas traces; those add waits and have
made the test machine unusable.

## Exact checkpoint

- Windows
- Legal `CUSA04301` game and firmware files
- Executable: `builds/cusa04301-full-covered-filled-20260829/shadps4.exe`
- SHA-256: `183D9914A395D8AD804428B418E13474D71172A6146386DD9E7D159F26AE14CC`
- Dedicated portable `user` directory
- No diagnostic `SHADPS4_*` environment flags for the baseline run

Do not point the experimental executable at another shadPS4 installation's user directory without a
verified save backup.

## Baseline cube test

1. Launch the checkpoint executable with the Dreams `eboot.bin` as its sole argument.
2. Enter DreamShaping and load the fixed scene containing one cube sculpt at `(0, 0, 0)`.
3. Enter edit mode and use the same three-side camera view shown in the checkpoint screenshot.
4. Record FPS, lighting, floor, cube coverage, cube fill, and whether sculpt/tweak-menu elements
   jitter while the camera moves.
5. Preserve the log and screenshot before another launch replaces them.

Expected checkpoint result:

- 30 FPS in the recorded view;
- normal lighting and edit floor;
- cube fully covered and fully filled;
- no observed sculpt or tweak-menu jitter;
- surface still wrong as a regular grid of rounded panels.

## Targeted CE3 surface trace

Use a fresh empty capture directory and relaunch the exact checkpoint with only:

```text
SHADPS4_DREAMS_CE3_FLECK_CAPTURE_DIR=<fresh absolute directory>
```

Do not enable the unrelated gather-focus trace.

1. Enter edit mode and place the cube/camera in the exact target view.
2. Create an empty `capture.request` file in the capture directory.
3. Wait for `complete.txt`; `failed.txt` indicates eight qualifying draws passed without a complete
   trace.
4. Preserve `ce3-fleck-trace.tsv/bin`, `atlas-neighborhood.tsv`,
   `atlas-neighborhood-r8.bin`, and `manifest.tsv`.

Interpret the result as follows:

- GPU sample differs from captured atlas interpolation: image view, sampler, or synchronization;
- samples match but discard is wrong: wave/EXEC/discard lowering;
- samples and discard match: VS370 parameters, association, or LOD.

The optional `SHADPS4_DREAMS_CE3_READCONST_CAPTURE=1` trace auto-arms on the first qualifying draw
after launch. Do not claim it belongs to the manually triggered cube draw unless the cube was that
first draw.

## Source reproduction

Apply both patches documented in [DEVELOPMENT.md](DEVELOPMENT.md) to upstream shadPS4 commit
`555c458c9fdd33cb4686492374519c7bb112a891` and its Sirit submodule revision `282083a`.

The cumulative patches are experimental and not upstream-ready.

## Save-space regression

1. Open Dreams' Limits Info page.
2. Record local usage, maximum blocks, creations, versions, and photos.
3. Create and save a small scene.
4. Confirm the emulator does not reproduce the old unsigned-underflow `4 GB used / 1 GB limit`
   result.
