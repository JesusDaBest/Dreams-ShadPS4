# Clean main build — October 25, 2026

**[Download the graphical launcher package](https://github.com/JesusDaBest/Dreams-ShadPS4/raw/refs/heads/main/builds/cusa04301-clean-main-20261025/Dreams-graphical-launcher-20261025.zip)**

Extract the complete ZIP, open **shadPS4QtLauncher.exe**, add your own Dreams game folder and click **Play**. The October 25 core and matching settings are selected automatically, as in the October 21 update. No command prompt, CMD, PowerShell, Python, Codex or Builder Bridge is required to play. Keep the extracted folder together; your own game/system setup is required.

This is the exact executable now selected by the author's main launcher:
`35cb8a3c407cb3b15b4e1fe05c10e127338ab4dd21aa251fc7ad0365cce7c921`.

## What changed

Dreams reused an allocation for geometry occupancy after it had held a lighting image. The emulator could copy that stale image back over the cleared and updated geometry bits. The resulting false occupancy blocked ambient light and made the tested Tren board almost black. A guarded correction invalidates the old image aliases after the verified complete raw clear, so later reads retain the new geometry data. It does not increase brightness or reduce a global shadow-strength setting.

This update also includes the retained October 23 texture-size, mip-layout, bounded raw-clear and shader corrections. Compared with that clean pre-debug build, only the rasterizer object changes for the new ambient-shadow correction. The custom right-click debug menu, live signal/resource capture tools and debug-control endpoint are excluded. Builder Bridge and its installers are excluded. Background control and runtime recorders are disabled; ordinary emulator diagnostics and earlier dormant diagnostic code remain in the core.

## Validation and limitations

- Six isolated real GPU cases and 22 guard checks passed with no Vulkan or synchronization validation errors. Coverage included stale-image corruption, empty geometry, all 256 occupancy patterns, existing visibility, later image reuse and a second clear.
- Live captures showed occupancy and ambient visibility recovering. The owner confirmed that the Tren board's colours were visible without new problems in that view.
- The clean executable contains the identical guarded shadow correction used in that private live test. It compiled and linked successfully; its startup, both main launch routes and the public Qt default route were verified. The clean executable itself has not received a separate full gameplay regression test.
- The new report of **very small shadows that can disappear at some viewing angles remains unresolved**. Its cause and relationship to earlier changes are not established. This publication deliberately preserves the current main executable before investigating it.
- Tren lag, intermittent black patches/white-floor flashes and other rendering problems are not claimed fixed. No consistent FPS improvement or slowdown was established by the shadow samples. Other GPUs, scenes and game versions remain unverified.

The test system used AMD Radeon 8060S and Dreams CUSA04301 with the established 2.64 content installation and Precise GPU readbacks. The package is experimental.

## Profiles and tutorial bypass

The launcher retains the previous graphical package's normal `%APPDATA%\shadPS4` profile policy. It backs up a different per-game configuration before applying the included settings. It does not move portable saves or replace creations. There are no game files, user saves, creations, accounts, firmware, credentials, captured resources or private session manifests in the package.

The optional **tutorial-skip/Skip Dreams Tutorial.exe** provides a graphical version of the previously tested progression helper. Close Dreams, open it, choose your own `CUSA04301/SYSTEM/savedata.json`, and confirm. It changes only the supported progression flags and creates an exact backup beside the save. It preserves the three optional voice-over flags and other data, and refuses unsupported formats. Its 15 synthetic checks passed; another player's tutorial outcome has not been live-verified. It is a bypass, not a repair of the tutorial engine.

## Source and packaging evidence

- [Checksums](builds/cusa04301-clean-main-20261025/SHA256SUMS.txt), [package checks](builds/cusa04301-clean-main-20261025/verification.json), and [runtime file manifest](builds/cusa04301-clean-main-20261025/runtime-manifest.json).
- [Source manifest](builds/cusa04301-clean-main-20261025/source-manifest.json) and [source delta from October 21](patches/dreams-clean-main-20261025.patch). Start from public base `b84847d1b6ca14d349bf9e11e91d14ae182e008d` and apply the manifest's patches in order. The previous 970 source-file hashes and 44 selected source/header overlays were checked, and the new patch applies cleanly. Machine-local `#line` filenames are made project-relative. A full fresh source rebuild is not claimed.
- The unchanged Qt launcher/runtime is taken from the verified October 21 package. Its [corresponding launcher source](builds/cusa04301-image-cache-20261021/qt-launcher-source-99e9829.tar.gz) and existing third-party notices/licenses remain applicable. The updated [settings adapter](builds/cusa04301-clean-main-20261025/DreamsCore.cs) and [optional tutorial helper](builds/cusa04301-clean-main-20261025/SkipDreamsTutorial.cs) are included as source.

Every ZIP member was allowlisted, hashed and checked. Publication does not restart the author's running game or modify their profile, saves or preserved Builder Bridge.
