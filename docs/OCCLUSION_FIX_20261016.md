# Dreams occlusion rendering fix — October 16, 2026

This download contains the exact executable currently played by the owner, SHA-256 `fd882da7acb151db2a149b25a8a108683e4ef5d076f24fdd8d636dbdeb1772be`.
Extract the ZIP and run **Launch-Dreams.cmd**, then enter your own Dreams eboot.bin path. It retains the preceding October 16 performance build's launch profile and fixes. No extra activation switch is required for this rendering correction. Requires your existing game/system setup, a Vulkan-capable GPU/driver and the Microsoft Visual C++ x64 runtime. No game, firmware, save or captured game-resource files are included.

## What changed

The Dreams traversal shader `0xb535c6c8` supplies array-style coordinates while sampling a plain 2D occlusion texture. The ignored slice still occupies an address component before the explicit mip. The old lowering consumed that slice as the mip and normalized mip-scaled coordinates against level-zero dimensions. This correction preserves the explicit mip and queries the corresponding dimensions. The traversal cache revision advances from 10 to 11.

This exact build also retains private, opt-in input-recording code in the rasterizer. The supplied launcher clears inherited SHADPS4 switches, uses the previous tested profile and does not arm captures. The incremental source patch includes this recorder so the published source matches the tested executable's source, rather than only the rendering hunk.

## Verification and limits

- The owner reported the corner corruption gone in the affected creation. The active GPU shader was checked for the corrected dynamic mip and matching dimension queries. Camera poses differ between earlier and later captures.
- The original production-function compiler test passes 65 descriptor/flag/LOD cases, including dynamic mip; the earlier lowering reproduced the lost mip. Other shader hashes retain their previous behavior in those tests.
- Publication reran that saved test executable and the packaged emulator's `--help`, checked the exact running-build hash, and reconstructed all 957 project-source files from the published patches. It did not perform a fresh full build or a new in-game test.
- Other scenes/GPUs and performance impact are unmeasured. The current shader's compiled mip cap is 7 while the saved raw guest descriptor reports last level 8; changed bound-view mip counts need further review. This exact owner-tested build is published with that limitation, without changing the executable being played.
- It retains the earlier shape correction, loading/readback/queue changes and pooled tiling allocations from the October 16 performance build. It retains that branch's older ordered-count implementation. It does not merge the separate October 6 branch or claim every historical fix is combined.

## Source

Start from public commit `b84847d1b6ca14d349bf9e11e91d14ae182e008d` on `dreams-dev-20260829-full-covered-filled`. Apply `patches/dreams-source-delta-20261016-performance.patch`, then `patches/dreams-occlusion-array-lod-20261016.patch`, in that order. Follow SOURCE_BUILD.md and the prior shape/performance build notes for prerequisites. Dependency trees retain the previous setup. The package source-manifest.json records both original Windows-byte hashes and LF-normalized hashes: Git patch application can normalize mixed CRLF/LF line endings without changing source text. Publication checked reconstructed text and restored original line endings before byte-hash comparison. validation.json records the bounded test results.
