# Dreams Development Snapshot

This branch is the complete experimental source used to investigate `Dreams` (`CUSA04301`) through
August 21, 2026. It is intended for emulator development, not normal gameplay. Read
[`DREAMS_HANDOFF_20260821.md`](DREAMS_HANDOFF_20260821.md) before changing the sculpt path.

## Exact status

- Dreams is **not playable** in this snapshot.
- Startup can reach offline menus, tutorial logic, DreamShaping, and creation scenes.
- UI, the imp, the grid, and gadgets can render; placed gadget logic has worked in testing.
- Sculpt and paint/fleck geometry is still missing or malformed. Characters are not confirmed.
- Tweak-menu surfaces can be black, generated 3D positions can jitter with camera movement, and the
  intro can be black with severely delayed audio.
- The earlier stable executable ran the tested creation scene at roughly 15-16 FPS but left
  sculpts and paint invisible.
- The malformed dark/grey cubes were caused by a wrong ordered-counter base calculation. The
  corrected calculation removes that corruption but does not yet restore the missing sculpt.
- The large sculpt root currently stops in the CPU scene-cache/model-record path before the paired
  record count is published to traversal.

Do not describe the August 17 full-screen/30 FPS observation as a verified general fix. Later
same-machine tests contradicted it.

## Confirmed corrections retained

- Startup and offline compatibility changes allow the game to pass the old Sony-logo/service
  blockers without live Dreams servers.
- AAC state reset and wall-clock video pacing address concrete decoder/pacing faults, although the
  complete intro path still regresses.
- Save-data free space now counts rounded 32 KiB save blocks and avoids unsigned underflow. This
  removed the false `4 GB used / 1 GB limit` state in the tested save.
- Shader support added during the investigation includes dynamic control flow, mask/lane
  operations, 64-bit GDS atomics, mixed descriptors, metadata images, and `DS_ORDERED_COUNT`
  translation.
- Compute pipelines that use ordered count are created with
  `VK_PIPELINE_CREATE_DISPATCH_BASE_BIT` before `vkCmdDispatchBase` is used. The previous
  combination was invalid Vulkan usage.

## Current upstream blocker

The active large sculpt root contains roughly 5,133–5,134 objects, but captured type-1 entries and
published paired records come only from small 52-object sky/menu roots. The large-root builder has
a sentinel cached identity and two empty incremental tables. With a nonzero token it can stay on an
empty incremental path instead of entering full initialization.

The opt-in conditional cache bootstrap in `src/core/module.cpp` and `src/core/signals.cpp` preserves
the original zero-token branch and admits the sentinel/empty-table state. It must keep the resource
rejection at guest `+0x8b7bc0`; bypassing that check installs incomplete state. This candidate
builds, but it is not yet a confirmed visual fix.

## Ordered count remains incomplete

Dreams traversal compute shader `0xb535c6c8` uses `DS_ORDERED_COUNT`. The current backend reduces it
to ordinary atomics and does not reproduce guest wave-creation ordering:

- M0 high bits are correctly preserved as a dword base. Only `OFFSET0` is divided by four.
- M0 low bits containing the logical wave ID/wave-crawler increment are currently discarded.
- `wave_release` and `wave_done` are decoded but do not control a guest-order queue.
- Splitting direct traversal into ordered host workgroup dispatches does not order multiple guest
  waves inside a workgroup.
- Indirect traversal still uses native indirect dispatch unless an expensive diagnostic path is
  enabled.

The captured shader has four ordered-count operations. All release the wave; the final operation
also marks it done. Revisit guest-wave ordering after the large root produces CPU records.

The dominant captured geometry draw was indexed-indirect vertex shader `0xd25db925`, fragment
shader `0x3f6e1a00`, `max_count=897`, stride 20, and approximately 46.3 ms in that diagnostic run.
This connects the malformed traversal output to the missing sculpt/fleck draw rather than proving
that the draw itself should be skipped or capped.

## Protect other games and user data

This snapshot contains emulator-wide changes and extensive diagnostics. Use it only in a dedicated
Dreams build. A local `user` folder can isolate settings, saves, trophies, logs, and caches from a
normal shadPS4 installation. Do not distribute game files, firmware, keys, user configuration, or
saves.

The diagnostics are disabled unless their environment variables or trigger files are supplied.
Many force GPU waits and can reduce performance to about 1 FPS, so diagnostic results are not valid
performance measurements.

## Required Sirit patch

The source uses `OpGroupNonUniformShuffle`, supplied as a separate patch because the Sirit
submodule was locally modified without a publishable gitlink commit:

```bash
git submodule update --init --recursive
git -C externals/sirit apply ../../patches/sirit-group-nonuniform-shuffle-20260821.patch
```

The corrected path is the default. Set
`SHADPS4_DREAMS_REPRO_CORRUPTED_ORDERED_BASE=1` before shader compilation only when intentionally
reproducing the earlier dark, fragmented, flickering cubes for an A/B comparison.

Current status, evidence, and priorities are maintained on the repository's `main` branch:

https://github.com/JesusDaBest/Dreams-ShadPS4
