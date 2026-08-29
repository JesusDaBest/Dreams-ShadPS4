# Open Issues

## 1. Correct the `0xce3b8413` sculpt atlas raymarch/coverage stage

The known cube is now a stable, filled volume, but its surface is a regular panel grid. Capture both
fragment-shader sample sites, exact atlas neighborhoods, and the discard decision for one fixed
draw. Compare the GPU samples with software interpolation before changing image or shader code.

## 2. Verify the final DCC decoder and materials

The fullscreen `0xdcc325c2` pass consumes the CE3 visibility/depth targets and writes the final
G-buffer. Investigate it after CE3 coverage is proven, especially for color or material errors.

## 3. Expand sculpt, paint, and character regression coverage

Confirm a sculpt preview, placed sculpt, paint stroke, character, and at least one premade scene.
They must remain visible and selectable while the camera and LOD change.

## 4. Preserve the fixed ordered-count path

Keep the exact `0x4ebeffd2` collect/prefix/replay result stable. Add focused regression coverage for
guest logical group order, final counter publication, cache revision, and repeated launches.

## 5. Measure performance without capture waits

The validated cube view reached 30 FPS, but other scenes have been much slower. Profile only after
disabling forced readbacks, capture waits, and diagnostic scheduler finishes.

## 6. Verify startup, lighting, floors, and save behavior

Repeat clean launches and confirm homespace lighting, edit floors, intro presentation, save loading,
and the corrected false-full save accounting remain stable.

## 7. Investigate remaining crashes independently

Capture a native stack and message for tutorial or gadget-selection crashes before assigning them
to the sculpt renderer.

## 8. Reduce diagnostics and isolate upstream-quality changes

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
