# Dreams sculpt/fleck investigation handoff — August 21, 2026

This is an experimental research checkpoint for `Dreams` (`CUSA04301`) on shadPS4. It is not a
playability claim or an upstream-ready fix. The snapshot intentionally keeps extensive opt-in
diagnostics so another developer can reproduce the current states and continue from the same
evidence.

## User-visible target and current status

The expected sculpted cube is Dreams' fleck/surface representation, with coherent planar faces and
edges. It is not meant to become a conventional opaque mesh cube.

Observed states during this investigation were:

- invisible sculpt/paint output;
- output appearing only after leaving sculpt mode;
- dark, fragmented cube-like output with moving flecks, flicker, and camera-relative jitter;
- reduced flicker after synchronization experiments, without correct shape or color; and
- a return to invisible sculpts after correcting the ordered-counter address.

UI, the imp, the grid, gadgets, sky/menu flecks, DreamShaping, and edit/sculpt mode can work. Sculpt
rendering is still unresolved. The corrupted cubes are useful regression evidence, not the desired
result and not a random visual seed.

## Reproducible corrupted-cube comparison

The normal path now preserves the ordered-counter base correctly. A deliberately wrong, opt-in
comparison path is retained in
`src/shader_recompiler/ir/passes/resource_tracking_pass.cpp`:

```text
SHADPS4_DREAMS_REPRO_CORRUPTED_ORDERED_BASE=1
```

Set the variable before the shader is compiled and use a fresh shader/pipeline cache. This restores
the earlier mistake of treating the already-dword M0 base as a byte address. It is expected to
produce the dark, fragmented, flickering cube-like corruption. Unset the variable and use another
fresh cache for the corrected A/B. Never treat the comparison path as a fix.

The historical comparison executable had SHA-256
`2980EDF5FF34C88F08EB334AD800AD2A7CE3C4B7A5DCE025A6D8651097E6D040`; no executable is
distributed in this repository.

## Confirmed ordered-counter address finding

For `DS_ORDERED_COUNT`, M0 high bits are already a dword base. The correct index is:

```text
((M0 >> 16) & 0xfffc) + (OFFSET0_bytes >> 2)
```

Preserve the first/base part, mask its ignored low two bits, and divide only the byte-encoded
`OFFSET0` selector by four. Do not combine both pieces and then divide the whole address. The wrong
formula remapped the ordered counters into unrelated guest GDS slots and exposed corrupted cubes.
The corrected path uses the expected `0x2400...` ordered-counter range in the captured scene.

This fixes address provenance only. It does not implement guest wave-creation ordering,
`wave_release`, or `wave_done`; those semantics may still matter after CPU records reach traversal.

## Where the active sculpt pipeline currently stops

The active large sculpt root exists, but it never reaches the paired render-record producer in the
captured baseline:

- active roots contained roughly 5,133–5,134 objects and were in phase 3;
- all 19,264 captured type-1 entries came from four 52-object sky/menu utility roots;
- none came from the large sculpt roots;
- the large-root builder retained cached identity `UINT64_MAX`;
- both 4,096-capacity incremental tables had active size zero and zero keys; and
- pair publication remained 18 utility records per frame, with no large-root records.

The producer reads the published count near guest `+0x78755a0` before traversal shader
`0xb535c6c8`. Do not force that count: the missing records are upstream of traversal and the final
draw in this state.

## Scene-cache bootstrap finding

The builder at guest `+0x8b7380` is called normally every frame, so scheduling is not excluding the
large root. The important gate is:

- root identity at `+0x1c2bf90` versus cached builder identity at `+0x1cbfc8`;
- readiness bytes at root `+0x27b0a9` and `+0x27b0b0`;
- token/generation at root `+0x27b0c0`; and
- original zero-token branch at guest `+0x8b74c0`, which enters full initialization at
  `+0x8b7b89`.

With a nonzero token, sentinel cached identity, and empty incremental tables, the original path can
loop through empty incremental work forever. The current source has an opt-in conditional bootstrap
under `SHADPS4_DREAMS_SCENE_READY_HANDOFF`. It preserves the original zero-token branch and adds a
full build only when:

- the token is nonzero;
- both readiness bytes are nonzero;
- cached identity is `UINT64_MAX`;
- root identity is valid; and
- both incremental active-size fields are zero.

The table layouts are pointer/capacity/size at builder `+0x243fe0/+0x243fe8/+0x243ff0` and
`+0x244000/+0x244008/+0x244010`.

The resource check at guest `+0x8b7bc0` must remain intact. A `0xffff` resource exits before the
builder resets tables or commits cached identity, so retrying next frame is safe. Cached identity is
committed only at `+0x8b8113`. Do not skip `+0x8b7bc0`.

Forced full initialization enumerated the large root but still produced no model output, so the
conditional bootstrap is a candidate repair, not a confirmed visual fix. The next focused trace
should record the first `+0x8b7bc0` resource rejection and its source object, then preserve the
original branch behavior.

## Readiness handoff

The focused `+0x9ab22e` scene-ready repair only covers a writer-equivalent phase-3 state where the
input is ready and either fallback input is absent. It was inert (`repaired=0`) in the captured
problem run. Keep it separate from the cache-bootstrap conclusion.

The global byte near `+0x8b9867` is a C++ local-static guard, not a renderer-enable flag. Never
force it.

## Behavior matrix

| State | Result | Meaning |
| --- | --- | --- |
| Earlier stable build | About 15–16 FPS; creation UI works; sculpts/paint invisible | Stable baseline, not a geometry fix |
| Wrong ordered base | Dark/grey fragmented cube-like flecks; flicker and camera jitter | Reproducible GDS corruption comparison |
| Correct ordered base | Corrupted cubes disappear; sculpt can be invisible | Address correction is necessary but insufficient |
| Forced full cache initialization | Large root enumerates and cache identity can install; output still stalls | At least one downstream CPU-record issue remains |
| Conditional cache bootstrap | Builds and reached edit mode in the closest pre-toggle candidate | Visual/result A/B still needs a clean recorded run |

## Build and dependency instructions

The source uses two uncommitted Sirit additions for `OpGroupNonUniformShuffle`. They cannot be
represented by a dirty submodule pointer, so this checkpoint includes a separate dependency patch:

```bash
git submodule update --init --recursive
git -C externals/sirit apply --check ../../patches/sirit-group-nonuniform-shuffle-20260821.patch
git -C externals/sirit apply ../../patches/sirit-group-nonuniform-shuffle-20260821.patch
```

The Sirit patch SHA-256 is
`F9DBF0B7C8BD4F43EC2104D576C238195F69178082C53C76E55E4C4F5E9C0D06`.

The complete source plus that dependency patch built successfully as `shadps4.exe` on August 21.
The local build artifact SHA-256 was
`9504016C9746A6A38A4F3C21B30AA8FACCA58F71D5C29272EADA34BD007953BF`; binaries are not
distributed here.

## Focused test environment

The latest tests used Windows, CUSA04301 1.00, an AMD Radeon 8060S, and an isolated Dreams launcher
version. These environment flags formed the main experimental baseline:

```text
SHADPS4_DREAMS_FAKE_PSN=offline
SHADPS4_DREAMS_SYNC_COMPUTE_RELEASE_MEM=1
SHADPS4_DREAMS_SCENE_COMPACT_ORDERED=1
SHADPS4_DREAMS_ORDERED_COUNT_BATCH=1
SHADPS4_DREAMS_GRAPHICS_BUFFER_STABILIZE=1
SHADPS4_DREAMS_COMPUTE_INDIRECT_STABILIZE=1
SHADPS4_DREAMS_VISIBILITY_LIST_ORDERED=1
SHADPS4_DREAMS_48A_LDS_BARRIER=1
SHADPS4_DREAMS_SCENE_READY_HANDOFF=1
```

Diagnostics can force GPU waits or write large logs. Enable only the focused trace needed for one
run, and do not use traced runs for performance measurements.

## Safe continuation order

1. Build this exact snapshot after applying the Sirit patch; record commit, executable hash, flags,
   and cache state.
2. Run the corrected ordered-base path first and verify the live breakpoint bytes before drawing a
   conclusion.
3. Test the conditional cache bootstrap without bypassing `+0x8b7bc0`.
4. Trace only state transitions: first resource rejection, cache commit, enumerated count, model
   output count, and paired-record publication.
5. Success means the large root contributes type-1 records and published records—not merely that
   more pixels appear.
6. If large-root CPU records appear but model output remains zero, follow the model/output builder
   before returning to the GPU ordered-count path.
7. Use `SHADPS4_DREAMS_REPRO_CORRUPTED_ORDERED_BASE=1` only for a same-scene, fresh-cache A/B
   against the known corrupted cubes.

Avoid manual readiness writes, broad branch inversion, forcing the published count, skipping
`+0x8b7bc0`, forcing the local-static guard, capping/remapping the large root, or calling malformed
output fixed.

## Important source locations

- `src/shader_recompiler/ir/passes/resource_tracking_pass.cpp`: ordered-counter address and the
  corrupted-cube A/B switch.
- `src/shader_recompiler/frontend/translate/data_share.cpp`: `DS_ORDERED_COUNT` translation.
- `src/shader_recompiler/backend/spirv/emit_spirv_atomic.cpp`: ordered-count emission.
- `src/video_core/renderer_vulkan/vk_compute_pipeline.cpp`: dispatch-base pipeline requirement.
- `src/core/module.cpp` and `src/core/signals.cpp`: opt-in CPU cache/readiness diagnostics and
  conditional repair.
- `src/video_core/renderer_vulkan/vk_rasterizer.cpp`: extensive experimental GPU tracing and
  ordering work; isolate before any upstream submission.

No game files, firmware, keys, credentials, saves, or proprietary Dreams content are included.
