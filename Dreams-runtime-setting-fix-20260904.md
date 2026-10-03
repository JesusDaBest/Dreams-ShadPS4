# Dreams patterned-cube behavior: configuration fix — September 4, 2026

**The Dreams-specific readback override was set to Relaxed (1), overriding the global Precise (2) setting. Changing it to Precise produced the tan patterned cube at30FPS in the isolated comparison, and the user confirmed smooth movement without sculpt jitter. The same setting is now applied to the normal launcher profile.**

The normal launcher subsequently started the preserved August29 executable, reached Homespace, and displayed the tan patterned cube with a30FPS overlay. Its startup log confirms `precise=true` and that the eager readback path was skipped. The normal session was left running for the user.

## Exact change

File: `C:\Users\Jdura\AppData\Roaming\shadPS4\custom_configs\CUSA04301.json`

`GPU.readbacks_mode`: **1 → 2** (Relaxed → Precise).

This was the only setting changed in the normal profile. Immediate presentation, pipeline-cache disabled, shader-dump disabled and the saved executable were retained. Existing saves were not replaced. The original per-game JSON was copied and verified before the atomic replacement; backup: `D:\CodexData\Workspaces\2026-09-04\usi\work\runtime-comparison-20260904\applied-20260904-180823\CUSA04301.before.json`.

The executable remains SHA256 `183D9914A395D8AD804428B418E13474D71172A6146386DD9E7D159F26AE14CC`. No new emulator binary or GPU workaround was introduced by this correction.

## Test and observations

Two separate portable profiles were cloned from the current profile:179 files,11,905,223 bytes, copied and hash-verified. Their effective game setting differed only in readback mode. Firmware/font paths were redirected to the existing installation equally in both; writable saves, caches and logs remained separate. Both used the same183D executable, Immediate presentation and disk caching disabled.

The Relaxed run enabled only the existing narrow CCA80 readback timer. In Relaxed mode this instruments the readback the build already performs. The Precise run had no diagnostic environment flags: enabling that timer there would force the readback back on and spoil the comparison. Each isolated run had a four-minute limit and ended through that watchdog; recorded exit code1 is the deliberate watchdog termination, not evidence of a game crash.

In Relaxed mode, the live Homespace view was black apart from the imp, with low initial FPS; the pause menu later reached30FPS and showed ghosted/duplicated imp imagery. Readback profiling produced227 reports with a median aggregate wait of130.202ms per reported interval. These values measure that path's wait overhead; they are not a controlled end-to-end performance benchmark.

The user navigated the Precise-mode clone into Edit Mode. The assistant observed the tan patterned cube/floor and30FPS, and the user answered **Yes** when asked whether it felt smooth without sculpt jitter. The setting was then applied to the normal profile, and the actual launcher startup and normal-session image were verified.

The two runs were not a scripted camera-identical benchmark, and a still image cannot prove absence of jitter. The user's movement feedback establishes the isolated-run experience. The normal-session image and logs establish that the corrected configuration is being used and the desired cube appearance/FPS returned. This is an observed configuration correction for this checkpoint, not proof that every Dreams scene or GPU instruction is correct.

## Why this setting matters in this build

The archived source has a Dreams-specific result-transfer path for shader`0xcca80e03`. With Relaxed readbacks it explicitly downloads GPU-written results, potentially fencing the Vulkan command stream. Precise mode uses the emulator's GPU-write tracking and skips that eager download. This is a concrete execution difference, and the current-setting run measured its overhead. The test does not separately establish the entire memory/timing chain that previously caused black/jittery surfaces.

Earlier checks correctly identified the saved executable but did not restore the successful behavior. A previous failed profile that already used Precise mode was not sufficient evidence to rule out a readback-setting correction in the current profile. The new isolated test and normal-launcher verification supersede the earlier unresolved-restoration reports for the observed cube result.

## Historical reference recovered

The original portable runtime still exists at `D:\Downloads\Dreams-shadPS4-Fix\isolated-sphere-test-20260819`. It contains the older **test** creation, saved August29 at10:34:10PDT, whose cover visibly shows the patterned cube. Its current-profile replacement contains different scene data and a black cover, despite reusing the internal creation/version IDs.

The original cache contains a B1`_doc1.spv` written at10:32:16, just before the successful10:33 screenshot, and configuration backups show Precise readbacks/Mailbox/cache enabled. Some files changed later, so this is a close historical reference, not an exact frozen runtime snapshot. **Those old saves/cache files were not installed to obtain the successful current-profile result.** They are being preserved separately for future GPU work.

## Evidence

- `Dreams-precise-isolated-20260904.png`: user-confirmed isolated result.
- `Dreams-normal-launcher-restored-20260904.png`: actual normal-launcher result.
- `Dreams-precise-readbacks-applied-20260904.json`: exact change, backup and verification record.
- `Dreams-runtime-fix-evidence-20260904.zip`: bounded logs, configurations, scripts and manifests; no user save data or game executable.


The two screenshots listed above are included at the root of `Dreams-runtime-fix-evidence-20260904.zip`, rather than as separate repository files.
