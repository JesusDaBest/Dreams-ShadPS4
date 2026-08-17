# Dreams on shadPS4 Investigation

This repository tracks a source-level investigation into running `Dreams` (`CUSA04301`) on
`shadPS4`.

## Current status - August 17, 2026

- **Not playable yet.** Do not describe the current build as a complete fix.
- Startup progressed from the old Sony-logo crash to the real Dreams intro, three startup screens,
  Preferences, and the opening tutorial path.
- A tutorial scene has run far enough to spawn and move the imp, but almost all scene geometry is
  black or confined to a tiny region at the top-left.
- The latest build still has a serious presentation/audio regression: the intro is not visible and
  its audio plays at roughly 1-2 FPS even though game timing is approximately correct.
- The strongest current rendering lead is an empty scene-publication queue before the Dreams CSG
  traversal shader. The traversal dispatch receives zero work, so fixing traversal ordering alone
  cannot make the missing scene appear yet.

## Most important findings

- Dreams depends on real `DS_ORDERED_COUNT` behavior. A basic shared atomic is not sufficient;
  ordering is defined by guest wave creation order.
- The queue producer shader `0x2bfebd3c` repeatedly receives `records=0` from its SRT count field.
- Its main queue and supporting buffers were confirmed zero in the failing scene.
- Traversal indirect dimensions are consequently zero, and the traversal GDS input total/base
  remain zero.
- GDS-to-memory release is working in the observed path. Captures at GDS offset `0xc00` contained
  live, increasing values, so the current evidence does not support a generic GDS release failure.
- An earlier Dreams workaround accidentally forced multiple full GPU waits per frame. Those waits
  were removed from normal execution, but another bottleneck still affects intro presentation and
  audio.

## Repository contents

- [STATUS.md](STATUS.md): current user-visible and technical state
- [DISCOVERIES.md](DISCOVERIES.md): detailed evidence and conclusions
- [FIXES_TRIED.md](FIXES_TRIED.md): fixes, experiments, and regressions
- [ISSUES.md](ISSUES.md): prioritized remaining blockers
- [REPRO.md](REPRO.md): reproduction and diagnostic notes
- `patches/dreams-focused-20260817-current.patch`: latest complete source snapshot
- `patches/`: older investigation snapshots retained for comparison

## Patch base

The August 17 snapshot is a cumulative working-tree patch against upstream shadPS4 commit
`555c458c9fdd33cb4686492374519c7bb112a891` and includes the local committed work plus the latest
uncommitted fixes and diagnostics.

This is an independent investigation repository, not an official shadPS4 release and not a
finished end-user build.
