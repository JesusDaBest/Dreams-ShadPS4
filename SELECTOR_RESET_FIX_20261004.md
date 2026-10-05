# Selector reset, pause/unpause lag and paint rendering — October 4, 2026

Dreams' selector compactor (`0x1e7dbccf`) and counter-reset shader (`0x723fd19c`) were incorrectly skipped by shadPS4's metadata-clear heuristic when reused guest addresses were registered as metadata. The compactor performs atomic allocations and writes record IDs. The reset shader initializes selector counters, GDS counters and dispatch dimensions; a metadata-only clear does not reproduce those effects.

The fix excludes these two shaders from that shortcut for CUSA04301, allowing their real compute dispatches to execute. This is a targeted compatibility correction, not a general rewrite of the metadata heuristic. No geometry or fleck count cap is imposed.

## Verified results

- Before the reset fix, pause/unpause increased the measured draw from 3,585 to 525,193 instances. Indexed geometry requests increased from 216,780 to 958,020,492. The draw cost rose from about 9 ms to 1,582–1,847 ms per frame.
- After the fix, the baseline had 14 active commands and 56 valid, distinct selectors. A later post-unpause capture had 8 commands and 41 valid, distinct selectors. Both had zero out-of-range IDs; these are different captured moments, not identical scene-state parity.
- The user confirmed that the severe pause/unpause lag stopped and paint strokes now render. The earlier producer capture had invalid IDs already at its output, so it did not prove a later overwrite.
- Instance counts are counts for this rendering pass, not a confirmed count of individual flecks or all objects in the scene.

The tested system is AMD Radeon 8060S with CUSA04301 VERSION 02.64 (APP_VER 01.00) and Precise GPU readbacks. GPU timestamp profiling was enabled during performance validation. Ordinary runs should leave diagnostic environment flags unset. Overall scene performance remains limited; this does not claim a general FPS fix.

## Current screenshot

Actual Dreams screenshot after the selector reset fix, with paint tools open. Paint rendering was confirmed separately by the user. This screenshot also shows remaining sculpt surface defects; it is not evidence that all fleck shapes, transparency or sculpt primitives are correct.

![Dreams after selector reset fix, with paint tools open](builds/cusa04301-selector-reset-fix-20261004/current-dreams-paint-mode.jpg)

## Build and source

- [Download tested executable](builds/cusa04301-selector-reset-fix-20261004/shadps4.exe), [hashes](builds/cusa04301-selector-reset-fix-20261004/SHA256SUMS.txt), [validation summary](builds/cusa04301-selector-reset-fix-20261004/validation.json).
- [Focused fix](patches/dreams-selector-reset-fix-20261004.patch).
- [Cumulative tracked main-source diagnostic snapshot](patches/dreams-diagnostic-source-20261004-selector-reset.patch), based on upstream `555c458c9fdd33cb4686492374519c7bb112a891`. This includes earlier corrections and diagnostic code; it excludes external submodules and untracked files. Retain the separate Sirit patch from the documented build workflow.
- Executable SHA-256: `DA5B2166B8245803E6D8F2BE4FC082B0CAB33E5D5366CE904118028BAB98C79C`.

The executable also retains the earlier cube alignment, imp/tweak-menu cache and scene-save corrections. Use it with the existing launcher/runtime dependencies; it is not a complete portable runtime package.

Remaining: verify sphere/cylinder previews and stamped shapes, individual fleck appearance and transparency, distance-dependent tweak-menu content, other slowdown triggers, GPUs and game versions. Nothing here establishes full Dreams compatibility. Raw proprietary shaders, buffer dumps and user logs are not included in this publication.
