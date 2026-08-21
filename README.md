# Dreams on shadPS4 Investigation

This repository tracks source-level work on `Dreams` (`CUSA04301`) in `shadPS4`.

## Current status — August 21, 2026

- **Not playable. Sculpt/fleck rendering is not fixed.**
- Startup can reach offline menus, tutorial logic, DreamShaping, creation scenes, edit mode, and
  sculpt mode.
- UI, the imp, the grid, gadgets, and sky/menu flecks can render.
- Sculpt output has ranged from invisible to dark, fragmented, flickering cube-like shapes with
  camera-relative jitter.
- The target is Dreams' coherent fleck/surface cube, not an ordinary opaque mesh cube.

## Latest concrete result

The corrupted cubes were traced to a wrong `DS_ORDERED_COUNT` address calculation. M0's high field
is already a dword base; only the byte-encoded `OFFSET0` selector is divided by four:

```text
((M0 >> 16) & 0xfffc) + (OFFSET0_bytes >> 2)
```

Dividing the combined value remapped ordered counters into unrelated guest GDS and produced the
dark/grey fragmented cubes. Correcting the address removes that corruption, but sculpts can become
invisible again. This proves the weird cubes were a deterministic bad-address comparison, not a
visual seed and not the final fix.

The current upstream blocker is earlier on the CPU. The active 5,133–5,134-object sculpt root is
present, but captured type-1 and paired records come only from small 52-object sky/menu roots. The
large-root scene-cache builder remains at sentinel identity with two empty incremental tables, so
the sculpt root never reaches the record count consumed by traversal shader `0xb535c6c8`.

An evidence-gated cache-bootstrap candidate is included. It retains the original zero-token full
build and adds only the ready + sentinel identity + empty tables case. The resource rejection at
guest `+0x8b7bc0` remains intact. The candidate builds, but it is not yet a confirmed visual fix.

## Reproducible weird-cube state

The source checkpoint keeps an intentionally wrong, off-by-default A/B switch:

```text
SHADPS4_DREAMS_REPRO_CORRUPTED_ORDERED_BASE=1
```

Set it before shader compilation and use a fresh shader/pipeline cache to reproduce the earlier
corrupted cubes. Unset it and use another fresh cache for the corrected path. The repository does
not distribute executables.

## Confirmed progress retained

- Offline startup/service compatibility reaches local content without recreating Dreams servers.
- Rounded 32 KiB save-block accounting removed the false `4 GB used / 1 GB limit` state in the
  tested save.
- Shader support includes dynamic control flow, mask/lane operations, mixed descriptors, metadata
  images, 64-bit GDS atomics, and `DS_ORDERED_COUNT` translation.
- Ordered-count compute pipelines use `VK_PIPELINE_CREATE_DISPATCH_BASE_BIT` before nonzero-base
  `vkCmdDispatchBase`; the prior Vulkan combination was invalid.
- The ordered-counter base now preserves M0's dword units.
- The CPU scene-cache sentinel/empty-table invariant and safe retry boundary are documented.

## Repository contents

- [HANDOFF_20260821.md](HANDOFF_20260821.md): complete August 21 evidence and continuation order
- [STATUS.md](STATUS.md): exact user-visible and build status
- [DISCOVERIES.md](DISCOVERIES.md): evidence, shader IDs, and technical conclusions
- [FIXES_TRIED.md](FIXES_TRIED.md): confirmed changes, experiments, and regressions
- [ISSUES.md](ISSUES.md): prioritized unresolved work
- [REPRO.md](REPRO.md): reproduction and diagnostic procedure
- [DEVELOPMENT.md](DEVELOPMENT.md): clone, patch, and build handoff
- `patches/dreams-focused-20260821-sculpt-handoff.patch`: cumulative experimental source patch
- `patches/sirit-group-nonuniform-shuffle-20260821.patch`: required Sirit dependency patch

## Development snapshot

The complete source checkpoint is on branch `dreams-dev-20260821-sculpt-handoff`. It includes the
corrected path, the opt-in weird-cube reproducer, the conditional CPU cache bootstrap, and all
gated diagnostics needed to continue. It is a research snapshot, not an upstream-ready patch.

The cumulative patch applies to upstream commit
`555c458c9fdd33cb4686492374519c7bb112a891`.

```text
dreams-focused-20260821-sculpt-handoff.patch
SHA-256: 43B4407F010D9E34930C035A2C0D15D6D626157B30D6593B2AD9D020D33249B8

sirit-group-nonuniform-shuffle-20260821.patch
SHA-256: F9DBF0B7C8BD4F43EC2104D576C238195F69178082C53C76E55E4C4F5E9C0D06
```

Both patches pass `git apply --check` against their documented bases. The combined source built
successfully as a Release executable. See [DEVELOPMENT.md](DEVELOPMENT.md) for exact commands.

This repository contains no game files, firmware, keys, PSN credentials, user saves, executables,
or proprietary Dreams content.
