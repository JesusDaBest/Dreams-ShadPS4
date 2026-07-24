# Dreams on shadPS4 Investigation

This repository tracks a live source-level investigation into getting `Dreams` (`CUSA04301`) running on `shadPS4`.

Current status:
- `Not playable yet`
- Intro-stage progress is real and repeatable
- The best Friday, July 24, 2026 baseline can survive the full automated harness window without crashing, although reconfirm runs are still flaky
- The current hard blocker is the post-intro / post-EOF handoff into real gameplay rendering

What is confirmed:
- Dreams does boot and execute well past the very first logo path
- Audio works
- Early flip/present behavior can be nudged forward with Dreams-specific synthetic flips
- Extending CPU-only synthetic bootstrap completion through the pre-AV tracked-source stage now looks like a real stability improvement
- The real VideoOut path can now use Dreams AV host-frame presentation after EOF when the tracked 10-bit source is blank
- The obvious EOF 10-bit guest-present candidates are both blank during the best stable runs, so the blocker is deeper than "wrong buffer chosen"
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
