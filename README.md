# Dreams on shadPS4 Investigation

This repository tracks a live source-level investigation into getting `Dreams` (`CUSA04301`) running on `shadPS4`.

Current status:
- `Not playable yet`
- Intro-stage progress is real and repeatable
- Audio and EOF AV host-frame presentation both work
- As of Tuesday, August 4, 2026, the main blocker is the post-EOF compute handoff around `0x853f753d` -> `0xff751373`
- The latest runs no longer fail at the old Sony-logo stage, but they still exit on the same guest-side read fault after the intro/EOF transition

What is confirmed:
- Dreams does boot and execute well past the very first logo path
- Audio works
- Early flip/present behavior can be nudged forward with Dreams-specific synthetic flips
- Extending CPU-only synthetic bootstrap completion through the pre-AV tracked-source stage now looks like a real stability improvement
- The real VideoOut path can now use Dreams AV host-frame presentation after EOF when the tracked 10-bit source is blank
- The obvious EOF 10-bit guest-present candidates are both blank during the best stable runs, so the blocker is deeper than "wrong buffer chosen"
- The post-EOF `0xff751373` tail shader can arrive with dummy buffers (`addr=0x1`) and dummy `1x1` images (`addr=0x0`)
- The post-EOF `0x853f753d` setup shader still has dummy buffers, but it does bind a real storage image (`R32G32Uint`, `8x8x48`) before the crash
- The remaining blocker is still emulator GPU/presentation logic, not "missing source code" or a simple config issue

What this repo contains:
- [STATUS.md](STATUS.md): the latest technical state
- [REPRO.md](REPRO.md): local repro notes
- [ISSUES.md](ISSUES.md): open blockers
- [FIXES_TRIED.md](FIXES_TRIED.md): what actually helped, what failed, and what was ruled out
- `patches/`: current local patch snapshots from the working shadPS4 source tree

What this repo is not:
- Not an official shadPS4 repository
- Not a finished Dreams build
- Not a guaranteed end-user fix

Best use:
1. Read [STATUS.md](STATUS.md).
2. Read [FIXES_TRIED.md](FIXES_TRIED.md).
3. Start from the Friday, July 24, 2026 CPU-only pre-AV bootstrap baseline instead of restarting from the earlier EOF crash experiments.
4. Use the focused patch snapshot in `patches/` only as a working investigation snapshot, not as a minimal polished patch series.
