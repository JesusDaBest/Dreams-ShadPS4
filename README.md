# Dreams on shadPS4 Investigation

This repository tracks source-level work on `Dreams` (`CUSA04301`) in `shadPS4`.

## Current status — August 30, 2026

- **Not yet playable and not a complete visual fix.**
- Dreams reaches offline menus, DreamShaping, saved scenes, and edit mode.
- The retained checkpoint renders the test cube fully covered and fully filled at 30 FPS in the
  validated view.
- Sculpt and tweak-menu jitter was absent in that run, but a later clean-cache run of the exact
  executable produced a dark cube with jitter. The visual result is not yet reproducible.
- The remaining sculpt is visually wrong: its surface is a regular grid of rounded panels rather
  than the intended Dreams flecks.
- The exact validated executable, screenshot, hashes, and investigation record are included in
  [`builds/cusa04301-full-covered-filled-20260829`](builds/cusa04301-full-covered-filled-20260829).

![Validated full-covered/full-filled checkpoint](builds/cusa04301-full-covered-filled-20260829/validated-full-covered-filled.png)

## Latest confirmed correction

Dreams compute shader `0x4ebeffd2` uses `DS_ORDERED_COUNT` while producing a sculpt seed stream.
Replacing that guest operation with a normal Vulkan atomic assigns output rows in host workgroup
arrival order and corrupts the stream.

The retained implementation performs a GPU collect pass, an exclusive prefix in guest logical
order, and a replay pass at the exact ordered offsets before publishing the final guest counter.
This changed the known cube from incomplete/unstable chunks into a full, stable volume and removed
the observed jitter. It is an emulator-ordering correction, not replacement geometry.

## Current highest-priority defect

The current visual chain is:

`ordered sculpt records -> VS 0x3706083c cuboid proxies -> FS 0xce3b8413 atlas raymarch/coverage/depth -> fullscreen VS 0xa33ab236 + FS 0xdcc325c2 decoder -> lighting`

The controlled August 30 scene stamped eleven primitives. Producer `0x2bfebd3c` received only eight
records: x=`0,1,3,6,7,8,9,10`. Sphere and both donuts were already absent before the visible VS,
CE3, DCC, and lighting stages. The next proof-producing step is therefore to trace the adjacent
ordered count/publication edges and find the first point where eleven becomes eight. The dark
material/jitter state remains a separate downstream or coherence investigation.

An A3 dynamic-constant prewarm was tested and rejected: the range reported
`registered_before=true`, proving it was already resident. That experiment is not part of the
checkpoint.

## Repository contents

- [HANDOFF_20260830.md](HANDOFF_20260830.md): current source of truth, controlled evidence,
  rejected tests, uncertainties, and next proof-producing step
- [STATUS.md](STATUS.md): exact visible state and build identity
- [DISCOVERIES.md](DISCOVERIES.md): evidence, shader IDs, and technical conclusions
- [FIXES_TRIED.md](FIXES_TRIED.md): retained corrections, experiments, and regressions
- [ISSUES.md](ISSUES.md): prioritized unresolved work
- [REPRO.md](REPRO.md): clean reproduction and focused capture procedure
- [DEVELOPMENT.md](DEVELOPMENT.md): exact patch and build continuation workflow
- [`builds/cusa04301-full-covered-filled-20260829`](builds/cusa04301-full-covered-filled-20260829):
  validated executable, screenshot, and checkpoint investigation
- [`patches/dreams-focused-20260829-full-covered-filled.patch`](patches/dreams-focused-20260829-full-covered-filled.patch):
  cumulative shadPS4 source patch
- [`patches/sirit-group-nonuniform-shuffle-20260829.patch`](patches/sirit-group-nonuniform-shuffle-20260829.patch):
  required Sirit submodule addition

The exact source/build tree is also preserved on branch
`dreams-dev-20260829-full-covered-filled` at commit
`b84847d1b6ca14d349bf9e11e91d14ae182e008d`. Its tree is identical to the validated local
checkpoint while its parent keeps the dedicated Dreams development history self-contained.

## Exact checkpoint identities

- Upstream shadPS4 base: `555c458c9fdd33cb4686492374519c7bb112a891`
- Main patch SHA-256: `CCD22A4B97E246A62FAFA71DFE40894DDA9C90DD52A6E020F583512AE30B29C4`
- Sirit patch SHA-256: `0F826E81BB003AD6A797F942CA6B3F51BDBBFB8C189B21B1C4885E6FEDBE2041`
- Executable SHA-256: `183D9914A395D8AD804428B418E13474D71172A6146386DD9E7D159F26AE14CC`

The patch pair was reapplied successfully in clean detached worktrees at the stated base revisions.
The source built successfully, and 69 offline tests ran: 68 passed and one optional real-capture
test was skipped because no capture path was supplied.

This repository contains no game files, firmware, keys, PSN credentials, user saves, or proprietary
Dreams content. The included executable is an experimental shadPS4 build for this investigation.
