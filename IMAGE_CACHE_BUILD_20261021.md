# Dreams image-cache update — October 21, 2026

Exact owner-played core: `bf9843bce836d3893c0dcb9059c931517f4a18da43b35f0332b89d3e4dfdae98`.

Extract **Dreams-graphical-launcher-20261021.zip**, open **shadPS4QtLauncher.exe**, add your own Dreams game folder, select Dreams and click Play. The October 21 patched core and tested CUSA04301 settings are selected automatically. No command prompt, separate Dreams entry-point program or command scripts are needed. Keep the extracted folder together; your own game/system setup is required. Existing saves remain in the normal shadPS4 profile.

## Changes and evidence

The latest change restores shared-boundary image-cache admission in ObtainBufferForImage and CreateBuffer, keeping compatible adjacent image ranges on a shared cached backing. It retains the current branch's staging reuse, GDS-readback boundary, dynamic graphics-read handling, render-pass/presentation dependencies and preceding shader corrections. This package is the currently running normal owner build, not the separate NVIDIA wave32/RGB565 experiments.

The saved isolated GPU test passed **156 content, dirty-range, lifetime and admission checks**, with zero validation errors. Its changed-layer case uploaded 11,022,336 bytes versus 88,080,384 for its test parent. That isolated result is not an in-game FPS claim. Saved live checks confirmed code/environment activation and shared cached backing for both tested atlas ranges; the owner reported correct visuals and movement. **Tren lag remains unresolved and a performance improvement has not been established.** Other scenes and GPUs remain unverified; NVIDIA rendering problems reported for the separate experimental packages are not claimed fixed here.

## Packaging and source

Publication verified the exact executable hash, source-overlay hashes, patch application, executable startup, Qt default-core route, automatic configuration/environment and every ZIP member. It did not restart the running game, rerun live captures, or rebuild the emulator. The matching Qt launcher source archive and licenses are included alongside this release as in the previous package. The C# settings adapter source is included in runtime/. Official shadPS4 updates do not automatically carry these experimental core changes.

Reconstruct source from public base `b84847d1b6ca14d349bf9e11e91d14ae182e008d` and apply the patches in source-manifest.json in order. The manifest records frozen overlays for the changed linked translation units. **Fresh full-build reproducibility has not been verified.** No game files, firmware, saves, captured resources or credentials are distributed.
