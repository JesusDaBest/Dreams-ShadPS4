# Status

## Game and build

- Title: `Dreams`
- Serial: `CUSA04301`
- Status date: August 19, 2026
- Playability: **not playable**
- Current source base: `555c458c9fdd33cb4686492374519c7bb112a891`
- Current installed experimental executable SHA-256:
  `3C357ED2473B5DFF14A4F2A60A3F7523BAC0FC5050950F7E0AC5E254CB3DFCD1`
- Earlier stable executable SHA-256:
  `7CE75CB1F1D6E1BD4657AB81E585B64144A85E8A9D98CF014131A08476F3F5E4`

The executable hashes identify local test artifacts only; binaries are not distributed here.

## What works

- The game passes the old startup crash and can reach the Dreams intro and startup pages.
- Earlier known-good runs displayed Continue, consent/EULA, and Preferences together.
- Offline startup can enter tutorial logic, DreamShaping, local creations, and premade content.
- The imp, UI, editing grid, and gadgets can render.
- Gadgets became placeable in later tests and their logic executed.
- Controller movement and UI sounds react even when the scene is visually incomplete.
- Corrected save-block accounting removed the false full-save condition in the tested save, allowing
  creation of another scene.

## What does not work

- Sculpt geometry and paint/fleck strokes are invisible or malformed.
- Characters have not been confirmed as visible in the current builds.
- Sculpt placement can make the stamp sound without producing selectable visible geometry.
- Tweak-menu panels can render black.
- Generated geometry can move or jitter relative to the camera.
- Some scenes show only a colored dot or thin strip at the top edge.
- The current dispatch-base candidate shows large grey stepped shapes instead of correct geometry.
- The intro can be invisible while audio plays at a severely delayed rate.
- A trigger-zone selection produced a host C++ exception (`0xe06d7363`), but the captured exception
  record is insufficient to identify the root cause.
- Online community content, historical servers, and PSN entitlement behavior are not implemented or
  verified.

## Performance observations

- The earlier stable creation-scene baseline was approximately 15-16 FPS with sculpts and paint
  still invisible.
- The current malformed-geometry candidate was approximately 12 FPS in the captured scene.
- A profiler capture measured one indexed-indirect sculpt/fleck geometry draw at approximately
  46.3 ms.
- Broad diagnostics can force a GPU wait repeatedly and reduce execution to roughly 1 FPS. Those
  runs must not be used to rate normal performance.
- A historical 30 FPS menu observation was not reproducible as a general 3D fix and is no longer
  treated as the current result.

## Current rendering conclusion

Restoring `VK_PIPELINE_CREATE_DISPATCH_BASE_BIT` changed the output from missing geometry to visible
but malformed geometry. This proves that the traversal path is producing and consuming geometry
data, but it does not prove that the generated records are correct.

The remaining high-confidence blocker is incomplete `DS_ORDERED_COUNT` behavior. The current
backend does not preserve guest wave-creation order and does not implement the release/done queue
semantics encoded by Dreams.

## Safety

The source contains no game data, firmware, keys, credentials, or saves. Some changes are
emulator-wide, so use an isolated portable user directory and do not test other games with this
snapshot until those changes are audited.
