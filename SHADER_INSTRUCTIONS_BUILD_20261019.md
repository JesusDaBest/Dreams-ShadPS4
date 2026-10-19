# Dreams shader-instruction update — October 19, 2026

Exact owner-played executable: `640a3ba83eba972702d25cbf9a7404134c09075f4ece098881b590dd1714f2ff`.

Extract **Dreams-shader-instructions-20261019.zip**, double-click **Launch-Dreams.cmd**, and enter your own Dreams eboot.bin path. The launcher applies the tested configuration and environment automatically, backing up the existing per-game configuration. No manual activation commands are needed.

## Changes

The latest correction fixes GCN bit-field extraction controls and over-wide fields, scalar carry/borrow and ADDK overflow, and floating-point class masks including signed zero and combined masks. Constant propagation follows the same rules. Shader-cache version 20 prevents reuse of old compiled shaders.

This executable also retains the later branch overlays for stateful ordered-count handling, 60 Hz rendering mode, frame-fence/write-range synchronization, segmented sculpt ordered copies, and preceding shape/occlusion/readback changes. The tested launch profile is included. This does not merge the separate October 6 tweak-menu branch or establish that every historical fix is included. The cumulative patch includes retained optional diagnostic code; the packaged profile does not enable capture recorders.

## Evidence and limits

Saved validation reports 176,412 GPU instruction cases, 10,098 CPU instruction cases, zero mismatches and zero shader validation errors. Thirteen full shader programs were preserved byte for byte and 7,776 cache identity checks passed. Live checks covered a single cube in Edit mode and the kitchen in Play mode on the owner's machine; the recorded feedback reports correct kitchen visuals and movement. This is not a complete shader audit or a controlled performance comparison. Other scenes/GPUs remain unverified. See validation.json and shader-audit-scope.json.

Publication checks the executable hash, 37 frozen source-overlay hashes, cumulative patch reconstruction, ZIP contents and packaged --help. It does not rebuild the emulator or rerun the saved GPU/live tests. Source reconstruction uses the previous public base/patch chain plus the frozen overlay; fresh full-build reproducibility remains unverified. The manifest distinguishes frozen overlay hashes from reconstructed project hashes.

## Matching source

Start at public base `b84847d1b6ca14d349bf9e11e91d14ae182e008d` and apply these patches in order:
- `patches/dreams-source-delta-20261016-performance.patch`
- `patches/dreams-occlusion-array-lod-20261016.patch`
- `patches/dreams-ordered-count-20261017.patch`
- `patches/dreams-shader-instructions-20261019.patch`

See SOURCE_BUILD.md for prerequisites. No game, firmware, saves, captured game resources or credentials are included. Requires your own Dreams/system setup, a Vulkan-capable driver and Microsoft Visual C++ x64 runtime. This is an experimental Windows core build. Official emulator updates do not automatically include these fixes.
