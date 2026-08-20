# Open Issues

## 1. Implement guest-ordered `DS_ORDERED_COUNT`

The current backend keeps the counter address but loses the M0 logical-wave fields and treats the
operation as a normal atomic. Preserve guest wave identity and implement `wave_release` and
`wave_done` without a GPU scheduling deadlock.

This is the primary blocker because restoring valid Vulkan dispatch-base behavior changed missing
geometry into visible but incorrectly positioned geometry.

## 2. Make sculpt and paint geometry stable and visible

Validate one fixed creation scene. A sculpt preview, placed sculpt, and paint stroke must remain
visible and selectable while the camera moves. Do not accept nonzero indirect command counts alone.

## 3. Preserve UI and gadget rendering

Tweak panels can be black and geometry can jitter. Any ordered-count correction must preserve the
currently working imp, grid, UI, gadget placement, and gadget logic.

## 4. Repair intro presentation and audio timing

The decoder produces frames, but some current runs show black video and severely delayed audio.
Test this separately from the geometry path with diagnostics disabled.

## 5. Investigate the trigger-zone host exception

Selecting a trigger zone reproduced host exception `0xe06d7363`. Capture the exception message and
native stack before assigning it to rendering, save data, or gadget logic.

## 6. Reduce diagnostics and isolate upstream-quality changes

The snapshot contains thousands of lines of gated tracing and several emulator-wide experiments.
Separate confirmed corrections from diagnostics before upstream review. Audit all generalized
changes against games other than Dreams.

## 7. Verify offline ownership and content boundaries

Local content can load without live Dreams servers, but PSN entitlement/demo behavior and community
content are not verified. Do not imply that offline stubs restore discontinued online services.

## Regression coverage

- false `4 GB used / 1 GB limit` save state;
- black Continue/EULA/Preferences pages;
- invisible or severely delayed intro;
- zero indirect geometry commands;
- nonzero commands with malformed positions;
- colored top-edge dot or compressed render strip;
- black tweak panels;
- angle-dependent grey lines;
- camera-relative geometry jitter;
- missing sculpt previews, placed sculpts, paint/flecks, and characters;
- gadget placement or logic regression;
- forced diagnostics reducing execution to roughly 1 FPS;
- post-EOF and gadget-selection crashes.
