# Clean main shadow update — October 25, 2026

**[Download the graphical launcher package](https://github.com/JesusDaBest/Dreams-ShadPS4/raw/refs/heads/main/builds/cusa04301-shadow-fix-20261025/Dreams-graphical-launcher-shadow-fix-20261025.zip)**

Extract the complete ZIP, open **shadPS4QtLauncher.exe**, add your own Dreams game folder and click **Play**. The corrected core and matching settings are selected automatically. No commands are needed.

## What changed

- Correct floating-point shadow bounds so negative values are handled correctly.
- Preserve the required order of the shadow sorting and cascade-partitioning operations, fixing the flicker seen in the cube test.
- Sample the selected mip's dimensions when refining and building the shadow pyramid. Higher mip levels previously lost valid shadow values, causing the tested cube's shadow to disappear at mid-distance.

The earlier ambient-shadow, texture-size, shader and sculpt corrections are retained. The new core is the selected clean main build, SHA-256 `399a8aff073bc669d559048f8071eb9e16e301efca23a40dc5928626b3a8b035`.

## Validation and known issues

The focused GPU checks passed: 195 shadow-bound cases, 330 ordering cases, 112 mip-sampling cases and 19 complete native-shader pyramid cases, with no validation errors. Twenty earlier compiled-shader comparisons remained unchanged. The old pyramid fault was reproduced from captured data; the corrected shader matched the deterministic reference.

In the live cube test, flickering stopped and the shadow remained visible in the previously failing mid-distance range. The clean build uses the same tested compiler/cache objects and identical shadow-dispatch changes. Startup and the direct and graphical launch routes passed verification; the clean binary has not received a separate full gameplay regression run.

Testing used AMD Radeon 8060S and Dreams CUSA04301 with version 2.64 content. Other GPUs and versions remain unverified. Tren slowdowns and intermittent black patches/white-floor flashes are still unresolved. No overall performance gain is claimed.

## Saves and tutorial bypass

The launcher uses the normal `%APPDATA%\shadPS4` profile. It backs up a different per-game configuration before applying the included settings; it does not move portable saves or replace creations. The download contains no games, firmware, user saves or creations.

If a tutorial blocks progress, close Dreams and open **tutorial-skip/Skip Dreams Tutorial.exe**. Select your own `CUSA04301/SYSTEM/savedata.json`. The unchanged helper backs up the file before changing supported progression flags.

## Source and packaging evidence

- [Checksums](builds/cusa04301-shadow-fix-20261025/SHA256SUMS.txt), [package verification](builds/cusa04301-shadow-fix-20261025/verification.json), and [runtime file manifest](builds/cusa04301-shadow-fix-20261025/runtime-manifest.json).
- [Source manifest](builds/cusa04301-shadow-fix-20261025/source-manifest.json) and [shadow correction patch](patches/dreams-shadow-distance-20261025.patch). Start from public base `b84847d1b6ca14d349bf9e11e91d14ae182e008d` and apply every listed patch in order, including the preceding clean-main patch. The new delta changes 13 source/header files. Patch application and source hashes were checked; a fresh full-source rebuild is not claimed.
- [Settings adapter source](builds/cusa04301-shadow-fix-20261025/DreamsCore.cs), [tutorial helper source](builds/cusa04301-shadow-fix-20261025/SkipDreamsTutorial.cs), and the unchanged [Qt launcher source](builds/cusa04301-image-cache-20261021/qt-launcher-source-99e9829.tar.gz). Existing third-party notices and licenses remain included.

All 45 ZIP members were allowlisted and verified against their hashes. The previous [October 25 package](builds/cusa04301-clean-main-20261025/) remains available.
