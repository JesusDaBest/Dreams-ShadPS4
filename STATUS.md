# Status

## Game and checkpoint

- Title: `Dreams`
- Serial/version tested: `CUSA04301` / `01.00`
- Status date: August 21, 2026
- Playability: **not playable**
- Cumulative patch base: `555c458c9fdd33cb4686492374519c7bb112a891`
- Source checkpoint commit: `40c0731d78272e727adc3af2072733395db4e8ad`
- Latest local Release build SHA-256:
  `9504016C9746A6A38A4F3C21B30AA8FACCA58F71D5C29272EADA34BD007953BF`
- Historical corrupted-cube executable SHA-256:
  `2980EDF5FF34C88F08EB334AD800AD2A7CE3C4B7A5DCE025A6D8651097E6D040`

Executable hashes identify local test artifacts only; binaries are not distributed here.

## What works

- The game can pass the old startup crash and reach offline menus and local content.
- DreamShaping, creation scenes, edit mode, and sculpt mode can be entered.
- The imp, UI, editing grid, gadgets, and sky/menu flecks can render.
- Gadget placement and logic have worked in tested scenes.
- Corrected rounded-block save accounting removed the false full-save state in the tested profile.
- The current source and the required Sirit changes compile successfully as a Release build.

## What does not work

- Sculpt and paint/fleck geometry is still missing or malformed.
- Characters are not confirmed in the current builds.
- A sculpt stamp can make the placement sound without producing correct visible/selectable output.
- The intentionally wrong ordered base produces dark/grey, fragmented, flickering cube-like output
  with camera-relative jitter.
- Correcting that address removes the corruption but can return the sculpt to invisibility.
- Tweak-menu panels can render black.
- The intro can be invisible while audio plays at a severely delayed rate.
- Trigger-zone selection reproduced host C++ exception `0xe06d7363`; its cause is unresolved.
- Online community content and historical Dreams services are not restored.

## Current pipeline conclusion

The malformed cubes are not evidence that a solid mesh path is required. Dreams should render a
coherent fleck/surface cube. The cubes were caused by treating the `DS_ORDERED_COUNT` M0 dword base
as bytes and dividing the combined address. The corrected formula preserves M0 and divides only
`OFFSET0`.

The active blocker in the captured baseline is before GPU traversal. A 5,133–5,134-object sculpt
root exists, but only 52-object utility roots reach the type-1/paired record path. The large-root
cache builder remains at sentinel identity with empty incremental tables, leaving the published
sculpt record count at zero.

The source contains a conservative conditional full-cache bootstrap for that exact state. It is a
candidate and has not yet produced a verified correct sculpt. `DS_ORDERED_COUNT` wave ordering also
remains incomplete and should be revisited after large-root CPU records exist.

## Behavior matrix

| Build/state | Visible result | Interpretation |
| --- | --- | --- |
| Earlier stable path | About 15–16 FPS; sculpts and paint invisible | Stable baseline only |
| Wrong ordered base | Dark/grey corrupted cubes, flicker, jitter | Deterministic bad GDS address |
| Correct ordered base | Corrupted cubes gone; sculpt can be invisible | Necessary correction, not sufficient |
| Forced full cache init | Large root enumerates; output records still stall | Downstream CPU construction remains |
| Conditional bootstrap | Builds and preserves safe resource rejection | Clean visual/result A/B still required |

## Performance observations

- Earlier stable creation-scene tests were approximately 15–16 FPS with invisible sculpts.
- A malformed-geometry candidate was approximately 12 FPS in one scene.
- One indexed-indirect fleck draw measured approximately 46.3 ms under diagnostics.
- Broad traces can force GPU waits and reduce execution to about 1 FPS; those runs are not valid
  performance measurements.

## Safety

The source contains no game data, firmware, keys, credentials, saves, executables, or proprietary
Dreams content. The checkpoint mixes emulator-wide experiments with extensive diagnostics, so use
an isolated user directory and do not treat it as an upstream-ready patch.
