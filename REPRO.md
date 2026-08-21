# Reproduction Notes

## Setup

- Windows
- Legal `CUSA04301` 1.00 game and firmware files
- Release build from `dreams-dev-20260821-sculpt-handoff`, or both published patches
- Required Sirit dependency patch applied after submodule initialization
- Isolated portable user/cache directory
- Offline baseline: `SHADPS4_DREAMS_FAKE_PSN=offline`

These settings describe the captured investigation. They are not all proven requirements.

## Corrected baseline test

1. Leave `SHADPS4_DREAMS_REPRO_CORRUPTED_ORDERED_BASE` unset.
2. Use a fresh shader/pipeline cache.
3. Launch Dreams with only the focused experimental flags being tested.
4. Enter DreamShaping, open one fixed saved scene, and enter edit mode.
5. Record the imp, grid, UI, gadgets, tweak panels, sky flecks, sculpt preview, placed sculpt, and
   paint stroke.
6. Rotate the camera without changing the scene and check whether any generated geometry moves or
   flickers.
7. Record source commit, executable hash, environment, cache state, and game log.

Expected checkpoint result is incomplete: UI/imp/grid/gadgets and sky flecks may render while the
sculpt is absent. The corrected ordered address should not reproduce the dark fragmented cubes.

## Weird/corrupted cube comparison

1. Start from the same source and scene.
2. Use a different fresh shader/pipeline cache.
3. Set:

```text
SHADPS4_DREAMS_REPRO_CORRUPTED_ORDERED_BASE=1
```

4. Launch, enter the same edit/sculpt state, and repeat the same camera movement.
5. Confirm whether the dark/grey fragmented, flickering cube-like output returns.

This switch intentionally treats the already-dword M0 base as bytes. It is a known-bad A/B and
must not be reported as a fix. The visual target is Dreams' coherent fleck/surface cube, not an
opaque solid mesh.

## Conditional cache-bootstrap test

The candidate is gated by `SHADPS4_DREAMS_SCENE_READY_HANDOFF`.

For one clean run, preserve guest `+0x8b7bc0` and observe:

- root identity and cached builder identity;
- readiness bytes and token/generation;
- both incremental table active sizes;
- the first resource rejection at `+0x8b7bc0`;
- cache identity commit at `+0x8b8113`;
- large-root enumerated count;
- model output count; and
- whether type-1/paired publication includes the large root.

The candidate only adds full initialization for a nonzero token, ready inputs, sentinel cached
identity, valid root identity, and two zero-sized incremental tables. A `0xffff` resource must take
the original exit so the next frame retries without committing incomplete state.

Success is not “more pixels.” Success requires the 5,133–5,134-object root to contribute CPU
records, followed by a stable, selectable sculpt that does not move with the camera.

## Main experimental flags

The latest focused baseline used:

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

Enable only one focused trace at a time. Many diagnostic paths force GPU completion and can reduce
the game to about 1 FPS.

## Save-space regression test

1. Open Dreams' Limits Info page.
2. Record local usage, maximum blocks, creations, versions, and photos.
3. Create and save a small scene.
4. Confirm that the game's 1 GiB quota is not confused with host free disk space.

The corrected emulator rounds each regular file to 32 KiB PS4 save blocks and clamps subtraction
at zero. It must not return the old unsigned-underflow `4 GB used` state.

## Patch verification

Apply the cumulative patch at upstream commit
`555c458c9fdd33cb4686492374519c7bb112a891`:

```bash
git apply --check patches/dreams-focused-20260821-sculpt-handoff.patch
git apply patches/dreams-focused-20260821-sculpt-handoff.patch
git submodule update --init --recursive
git -C externals/sirit apply --check ../../patches/sirit-group-nonuniform-shuffle-20260821.patch
git -C externals/sirit apply ../../patches/sirit-group-nonuniform-shuffle-20260821.patch
```

```text
Cumulative patch SHA-256:
43B4407F010D9E34930C035A2C0D15D6D626157B30D6593B2AD9D020D33249B8

Sirit patch SHA-256:
F9DBF0B7C8BD4F43EC2104D576C238195F69178082C53C76E55E4C4F5E9C0D06
```

The patches are cumulative, experimental, and not upstream-ready.
