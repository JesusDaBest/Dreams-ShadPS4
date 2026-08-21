# Open Issues

## 1. Find the first large-root scene-cache resource rejection

The 5,133–5,134-object sculpt root is scheduled, but its builder remains at sentinel identity with
two empty incremental tables. Test the conditional cache bootstrap while preserving guest
`+0x8b7bc0`. Log the first `0xffff` resource and follow its object/resource provenance.

Do not skip the check: it exits before cache mutation and makes next-frame retry safe.

## 2. Make the large root publish CPU render records

All captured type-1 entries and paired records came from 52-object sky/menu roots. The large root
must reach the model/output builder and contribute records before GPU traversal can render it.
Validate cache commit, enumerated count, model output count, and paired publication in that order.

## 3. Complete guest-ordered `DS_ORDERED_COUNT`

The counter address is now correct: M0's high field is a dword base and only `OFFSET0` is divided by
four. The backend still loses M0 logical-wave fields and treats release/done as ordinary atomics.
Implement guest wave ordering without a GPU scheduling deadlock after the CPU producer is live.

## 4. Make sculpt and paint geometry stable and selectable

Validate one fixed scene. A sculpt preview, placed sculpt, and paint stroke must remain visible and
selectable while the camera moves. Do not accept nonzero command counts or corrupted cube pixels
as success.

## 5. Preserve UI, gadget, and sky-fleck rendering

Any producer, cache, or ordered-count change must preserve the currently working imp, grid, UI,
gadget placement/logic, and sky/menu flecks. Tweak panels must not become black.

## 6. Repair intro presentation and audio timing

The decoder produces frames, but some runs show black video and severely delayed audio. Test this
separately from sculpt rendering with forced-wait diagnostics disabled.

## 7. Investigate the trigger-zone host exception

Selecting a trigger zone reproduced host exception `0xe06d7363`. Capture the exception message and
native stack before assigning it to rendering, save data, or gadget logic.

## 8. Split research diagnostics from production fixes

The checkpoint mixes confirmed corrections with thousands of lines of gated breakpoints, captures,
waits, and title-specific experiments. Separate generic fixes from diagnostic code before any
upstream submission. The Sirit shuffle addition also needs a real dependency fork/commit rather
than a standalone patch.

## 9. Verify offline ownership and content boundaries

Local content can load without live Dreams servers, but PSN entitlement/demo behavior and community
content are not verified. Do not imply that offline stubs restore discontinued online services.

## Regression coverage

- false `4 GB used / 1 GB limit` save state;
- black Continue/EULA/Preferences pages;
- invisible or severely delayed intro;
- sentinel cache identity with empty incremental tables;
- large root absent from type-1 and paired records;
- zero indirect geometry commands;
- correct address with invisible sculpt;
- intentionally wrong address with dark/fragmented/flickering cubes;
- colored top-edge dot or compressed render strip;
- black tweak panels;
- camera-relative jitter;
- missing sculpt preview, placed sculpt, paint/flecks, and characters;
- gadget or sky-fleck regression;
- forced diagnostics reducing execution to roughly 1 FPS; and
- post-EOF or gadget-selection crashes.
