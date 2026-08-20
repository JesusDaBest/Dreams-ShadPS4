# Dreams on shadPS4 Investigation

This repository tracks source-level work on `Dreams` (`CUSA04301`) in `shadPS4`.

## Current status - August 19, 2026

- **Not playable. There is no complete 3D-rendering fix.**
- Startup can reach offline menus, tutorial logic, DreamShaping, and creation scenes.
- UI, the imp, the grid, and gadgets can render. Gadget placement and logic have worked.
- Sculpt, paint/fleck, and character geometry remains missing or malformed.
- Tweak panels can be black, 3D positions can jitter with the camera, and the intro can regress to
  black video with severely delayed audio.
- The August 17 statement that full-screen 3D was fixed at 30 FPS is withdrawn. Later repeat tests
  on the same machine contradicted it.

## Latest concrete result

The August 19 candidate restores the Vulkan dispatch-base pipeline flag required by Dreams'
ordered traversal path. Before this correction, `vkCmdDispatchBase` was used with nonzero base
groups on an incompatible compute pipeline. After the correction, previously invisible geometry
appeared, but as oversized grey shapes whose positions jitter between frames.

That result localizes the main failure to ordered sculpt/fleck geometry generation. It is a real
Vulkan correctness correction, but **not** a visible rendering fix.

Dreams compute shader `0xb535c6c8` uses `DS_ORDERED_COUNT`. The current implementation tracks the
counter address but reduces the operation to ordinary atomics; it does not preserve guest
wave-creation order or apply the instruction's release/done queue behavior. This is the primary
known rendering blocker.

## Confirmed progress retained

- Offline startup/service compatibility reaches local content without recreating Dreams servers.
- The false `4 GB used / 1 GB limit` state was traced to save-block accounting and unsigned
  underflow; rounded 32 KiB block accounting removed it in the tested save.
- The shader recompiler now handles several Dreams paths that previously failed translation,
  including dynamic control flow, lane/mask operations, mixed descriptors, metadata images,
  64-bit GDS atomics, and `DS_ORDERED_COUNT` translation.
- Expensive diagnostic GPU waits were identified as the cause of many 1 FPS tests. They remain
  opt-in and are not valid performance measurements.

## Repository contents

- [STATUS.md](STATUS.md): exact user-visible and build status
- [DISCOVERIES.md](DISCOVERIES.md): evidence, shader IDs, and technical conclusions
- [FIXES_TRIED.md](FIXES_TRIED.md): confirmed changes, experiments, and regressions
- [ISSUES.md](ISSUES.md): prioritized unresolved work
- [REPRO.md](REPRO.md): reproduction and diagnostic procedure
- [DEVELOPMENT.md](DEVELOPMENT.md): how another developer can continue safely
- `patches/dreams-focused-20260819-experimental.patch`: cumulative experimental source patch

## Development snapshot

The current experimental source is published separately from this documentation branch so another
developer can build and continue the same investigation state. It includes extensive gated
diagnostics and emulator-wide changes; it must not be treated as an upstream-ready patch or used as
a general shadPS4 build.

The cumulative August 19 patch applies to upstream commit
`555c458c9fdd33cb4686492374519c7bb112a891`. Its SHA-256 is
`CCAE708A596A9F40833E027A12723C5698452CA8596FD0E1DE1D1F9F4E1B5E60`.

This repository contains no game files, firmware, keys, PSN credentials, user saves, or proprietary
Dreams content.
