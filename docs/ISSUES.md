# Open issues — October 4, 2026

The tiled cube/floor normal seams were fixed and verified in the October 4 alignment build. The earlier imp trails and tweak-menu smush were fixed on the local system. Those resolved symptoms should not be treated as the current task list.

1. Test a sphere and more complex sculpts with the alignment fix; verify shape, shading, stamping, and deletion.
2. Investigate sculpt-limit warnings, remaining low frame rates and other editing slowdowns. The selector-reset correction resolved the demonstrated pause/unpause workload explosion and restored paint rendering on the tested system; broader performance remains unverified.
3. Correct tweak-menu content that renders differently depending on viewing distance.
4. Verify other GPUs and game versions, including the separate user's persistent imp trails and 02.65 installation.

See [the latest build and validation](SELECTOR_RESET_FIX_20261004.md).

## Historical investigation tasks

The tasks below are preserved from August/September. They require reassessment against the latest build before being resumed; they are not a current diagnosis of the corrected cube seams.

> **Latest September 4 runtime test:** [CLEAN_CORE_TEST_20260904.md](CLEAN_CORE_TEST_20260904.md) records two actual official-core runs with the general GPU corrections. Both fail translating `DS_ORDERED_COUNT` in `cbac06d2`; Dreams is not working on this build. This supersedes the earlier plan to establish a clean-core run. Earlier GPU provenance and correction tests remain in [HANDOFF_20260904.md](HANDOFF_20260904.md).

## 1. Find why the scene-record producer publishes zero or an incomplete prefix

Capture flattened SRT words 41 and 18 immediately before queue producer `0x2bfebd3c`. Refresh word
41 once through the existing focused readback to distinguish stale host/SRT state from a genuinely
empty producer. If input is nonzero, capture the producer predicate, ordered payload, returned
prefix, computed total, and indirect X publication.

## 2. Resolve Liverpool `DS_ORDERED_COUNT` address units

The September 2 x4 address family is not verified and contradicts the committed semantic tests.
The user experienced severe lag in that build, but the run does not prove address units caused it.
Restore the better-supported pre-x4 interpretation, then capture both candidate slots and run a
separate warm-cache performance A/B. Generic lowering and every exact replay path must use the same
proven unit.

## 3. Correct the `0xce3b8413` sculpt atlas raymarch/coverage stage

The known cube is now a stable, filled volume, but its surface is a regular panel grid. Capture both
fragment-shader sample sites, exact atlas neighborhoods, and the discard decision for one fixed
draw. Compare the GPU samples with software interpolation before changing image or shader code.

## 4. Verify the final DCC decoder and materials

The fullscreen `0xdcc325c2` pass consumes the CE3 visibility/depth targets and writes the final
G-buffer. Investigate it after CE3 coverage is proven, especially for color or material errors.

## 5. Expand sculpt, paint, and character regression coverage

Confirm a sculpt preview, placed sculpt, paint stroke, character, and at least one premade scene.
They must remain visible and selectable while the camera and LOD change.

## 6. Preserve the fixed ordered-count path

Keep the exact `0x4ebeffd2` collect/prefix/replay result stable. Add focused regression coverage for
guest logical group order, final counter publication, cache revision, and repeated launches.

## 7. Measure performance without capture waits

The validated cube view reached 30 FPS, but other scenes have been much slower. Profile only after
disabling forced readbacks, capture waits, and diagnostic scheduler finishes.

## 8. Verify startup, lighting, floors, and save behavior

Repeat clean launches and confirm homespace lighting, edit floors, intro presentation, save loading,
and the corrected false-full save accounting remain stable.

## 9. Investigate remaining crashes independently

Capture a native stack and message for tutorial or gadget-selection crashes before assigning them
to the sculpt renderer.

## 10. Reduce diagnostics and isolate upstream-quality changes

Separate title-specific investigation code, general GPU-emulation corrections, and one-shot capture
facilities. Audit generalized changes against other shadPS4 games before upstream review.

## Regression checklist

- full-covered/full-filled known cube remains stable;
- no sculpt or tweak-menu jitter;
- no black cube, missing preview, or disappearing placed sculpt;
- normal homespace lights and edit floor;
- no camera-distance chunk loss;
- paint/fleck and character visibility;
- UI, imp, grid, gadgets, and tweak panels preserved;
- no forced diagnostic path in performance runs;
- no device loss, out-of-memory error, or repeat-launch cache regression.
