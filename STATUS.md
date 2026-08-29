# Status

## Game and build

- Title: `Dreams`
- Serial: `CUSA04301`
- Status date: August 29, 2026
- Playability: **not playable; major visual defects remain**
- Source base: `555c458c9fdd33cb4686492374519c7bb112a891`
- Validated executable SHA-256:
  `183D9914A395D8AD804428B418E13474D71172A6146386DD9E7D159F26AE14CC`
- Checkpoint directory:
  `builds/cusa04301-full-covered-filled-20260829`

## What is confirmed working in the checkpoint

- Offline startup reaches local menus, DreamShaping, saved scenes, and edit mode.
- UI, imp, edit grid, floor, and normal homespace lighting rendered in the validated run.
- A single cube sculpt rendered with a full outer volume: all visible sides were covered and filled.
- Sculpt and tweak-menu jitter was absent.
- The recorded edit-mode view ran at 30 FPS with broad diagnostics disabled.
- The exact `0x4ebeffd2` ordered-count collect/prefix/replay path ran without fallback, Vulkan
  validation, device-loss, or out-of-memory errors.

## What remains wrong

- The sculpt surface is a regular grid of rounded panels instead of the intended Dreams flecks.
- Correct paint strokes, complex sculpts, characters, tutorials, and premade Dreams are not yet
  regression-confirmed on this checkpoint.
- Other scenes and camera distances can still expose culling, LOD, material, or performance issues.
- The experimental source contains extensive diagnostics and title-specific paths and is not yet an
  upstream-ready general shadPS4 change.
- Online community content, historical Dreams servers, and PSN entitlement behavior are not
  implemented or verified.

## Confirmed rendering conclusion

The B1 seed-writer shader `0x4ebeffd2` required guest-logical `DS_ORDERED_COUNT` allocation rather
than host atomic arrival order. Exact GPU collect/prefix/replay fixed the missing-volume and jitter
symptoms in the known scene.

The remaining panel pattern is downstream of that progress. Current evidence points first to
fragment shader `0xce3b8413`, which raymarches the sculpt atlas and decides coverage/depth for the
fixed cuboid proxy instances. The following fullscreen DCC decoder is a secondary target for color
or material errors.

## Rejected current hypothesis

The A3 (`0xa3a9e9ef`) dynamic ReadConst range was explicitly prewarmed for one falsification run.
The range was already registered before the prewarm. The experiment was reverted and must not be
described as part of the visible improvement.

## Verification record

- Exact executable size: `70,621,696` bytes.
- Release compilation succeeded with one parallel job.
- Patch pair reapplied cleanly to fresh worktrees.
- `git diff --check` passed for the checkpoint source.
- 69 tests ran: 68 passed and one optional real-capture test skipped.
- Validation screenshot SHA-256:
  `327C9E07B595A4E113C69ADF555F8928A804D676B0BFE912B651D822DF09ECB8`.

## Safety

Use an isolated portable `user` directory and preserve a save backup. The checkpoint includes no
game content, firmware, keys, credentials, or user saves.
