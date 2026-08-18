# Dreams on shadPS4 Investigation

This repository tracks a source-level investigation into running `Dreams` (`CUSA04301`) on
`shadPS4`.

## Current status - August 17, 2026

- **Not playable yet.** Do not describe the current build as a complete fix.
- Startup progressed from the old Sony-logo crash to the real Dreams intro, three startup screens,
  Preferences, and the opening tutorial path.
- Full-screen 3D rendering is now working in the tested offline content: flecks, characters,
  backgrounds, UI, and the imp rendered correctly at the game's 30 FPS cap.
- The fix is not scene-specific. It repairs missing Liverpool workgroup information and several
  shader lane/mask operations that feed Dreams' indirect geometry pipeline.
- Broader tutorial, sculpting, creation, and gameplay testing is still required before assigning a
  playable rating.

## Most important findings

- Removing invalid `V_READLANE_B32` mask-shadow restoration fixed the black/compressed top-edge
  presentation while preserving normal numeric lane reads.
- `V_WRITELANE_B32` now updates only the selected lane instead of broadcasting the write.
- The sprite compaction shaders now allocate one contiguous range per subgroup rather than leaving
  holes between active records.
- The indirect-argument shader now reads scalar lane masks coherently and receives the hardware
  workgroup-info SGPR requested through `COMPUTE_PGM_RSRC2.TG_SIZE_EN`.
- That shader changed from 897 zero commands to 897 nonzero commands, 504 with active instances.
- A clean run visually confirmed complete 3D menu content at 30 FPS; expensive diagnostics still
  reduce performance and must stay disabled for normal testing.

## Repository contents

- [DEVELOPMENT.md](DEVELOPMENT.md): easiest way to copy the current source and continue work
- [STATUS.md](STATUS.md): current user-visible and technical state
- [DISCOVERIES.md](DISCOVERIES.md): detailed evidence and conclusions
- [FIXES_TRIED.md](FIXES_TRIED.md): fixes, experiments, and regressions
- [ISSUES.md](ISSUES.md): prioritized remaining blockers
- [REPRO.md](REPRO.md): reproduction and diagnostic notes
- `patches/dreams-focused-20260817-3d-rendering.patch`: latest complete source snapshot
- `patches/`: older investigation snapshots retained for comparison

## Continue development

The [`dreams-dev-3d-rendering`](https://github.com/JesusDaBest/Dreams-ShadPS4/tree/dreams-dev-3d-rendering) branch contains a
complete standalone copy of the current patched shadPS4 source. Developers can fork it or clone it
directly and make changes in their own copy:

```bash
git clone --branch dreams-dev-3d-rendering --single-branch https://github.com/JesusDaBest/Dreams-ShadPS4.git
```

The current source snapshot is commit `3e5e23a54d339fdff53f619aece871737426c5bf`. Using the branch
is easier than manually applying the cumulative patch.

This custom branch includes emulator-wide changes. Use a dedicated portable `user` folder and run
the build only with Dreams; see [DEVELOPMENT.md](DEVELOPMENT.md#keep-other-games-isolated).

## Patch base

The August 17 3D-rendering snapshot is a cumulative patch against upstream shadPS4 commit
`555c458c9fdd33cb4686492374519c7bb112a891`. Its SHA-256 is
`60CE111B39ECE98D2D6FB30AA1C106956965976946D94BEF1A5EC98762E08BF2`.

This is an independent investigation repository, not an official shadPS4 release and not a
finished end-user build.
