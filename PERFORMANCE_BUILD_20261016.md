# Dreams performance build — October 16, 2026

This is the exact pooled-tiling executable currently used by the owner, SHA-256 `061b1aa078be0fe93d9a6f43a1fd3affe3880ec3ecab6041cecbe699a68ac347`.

Download the ZIP linked on the front page, extract it, and run `Launch-Dreams.cmd`. Enter the path to your own Dreams eboot.bin. Requires Windows, a Vulkan-capable GPU/driver, the Microsoft Visual C++ x64 runtime and your existing shadPS4 game/system setup. No game, firmware or save files are included.

The launcher applies the tested CUSA04301 configuration, backing up an existing per-game configuration first. It uses the existing normal shadPS4 profile and saves. Settings include Precise GPU readbacks, pipeline caching off, shader patching off, and the saved Dreams-specific environment switches. Environment changes apply only to the child emulator process. The package creates a fresh native queue-mode directory without arming a capture.

## Changes and limits

This succeeds the October 15 shape-fix executable and retains that branch's density-generation correction. It adds the retained scene-readiness, numeric-table, precise SRT, fair-queue and CPU read/write prefetch changes, plus pooled tiling scratch allocations. It retains the older ordered-count implementation. It does not merge the separate October 6 branch or claim that every historical fix is included.

With pooling enabled, tiling scratch buffers permit normal VMA suballocation rather than forcing dedicated allocations; VMA retains its automatic dedicated fallback. The owner reported substantially better performance, with existing visual bugs still present. Short comparison windows showed lower typical update times but variable memory availability and a remaining long sample; no precise speedup or complete stutter fix is claimed. Other GPUs and complete visual correctness remain unverified.

The saved production-function GPU test passed dedicated, pooled and other-title cases with zero validation errors. Publication verifies the exact EXE hash, source reconstruction and ZIP contents; it does not repeat a full build or new in-game test.

## Matching source

Apply `patches/dreams-source-delta-20261016-performance.patch` to public commit `b84847d1b6ca14d349bf9e11e91d14ae182e008d` on `dreams-dev-20260829-full-covered-filled`, then follow the Windows build prerequisites in SOURCE_BUILD.md and the October 15 build record. This cumulative project-source delta includes the previous shape correction; do not also apply the older delta. Dependency trees retain the previous source setup. Project hashes are in the build's source-manifest.json. The optional diagnostic/custom-rendering code remains in source but is not enabled by this launch profile.
