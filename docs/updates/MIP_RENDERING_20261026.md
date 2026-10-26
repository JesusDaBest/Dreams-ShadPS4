# Rendering update — October 26, 2026

**[Download the graphical launcher package](https://github.com/JesusDaBest/Dreams-ShadPS4/raw/refs/heads/main/builds/cusa04301-mip-fixes-20261026/Dreams-graphical-launcher-mip-fixes-20261026.zip)**

Extract the complete ZIP, open **shadPS4QtLauncher.exe**, add your own Dreams game folder and click **Play**. The corrected core and matching settings are selected automatically; no commands are needed.

## Changes

- Retain the earlier shadow repairs and the Light gadget correction that keeps its shadow visible after enabling Cast Shadows.
- Correct mip/detail selection and sampler settings, including the verified visibility-shader paths.
- Prevent size-arithmetic overflow and incorrect compressed-image edge copies. Preserve the selected mip and array layer during copies and resolves.
- Preserve the requested texture view when cached images overlap, are replaced or are rebound. Reject reversed mip ranges and incompatible shader-binding layouts.

The update also includes the tested storage-LOD and multisample-array read/view support. These are rendering-correctness changes; no general FPS or memory-saving improvement is established. In particular, the size-overflow repair prevents under-allocation rather than reducing texture memory.

Core SHA-256: `27657738f414bf2808f31b854a56fad1f1c1f9422920c8d11c42008568147e3f`.

## Validation and limits

The included fixes have focused CPU/GPU checks. The latest cache-view correction passed 864 selection cases and 864 native GPU-view cases; descriptor and binding-layout guards passed 90,176 descriptor checks, 2,166 cache comparisons and 32 native shader cases, with 26 existing shader comparisons unchanged. Relevant GPU tests reported no validation errors.

Earlier live checks confirmed the Light gadget's persistent shadow and correct moving views in Tren. The latest combined private build passed executable/profile checks and 203 supported-format texture allocation lower-bound checks, and continued updating frames. Its scene changed during timing, so that sample is not a performance comparison. The latest combined build has no additional user visual confirmation.

The clean package reuses 16 changed rendering objects from that private build. Its unchanged rasterizer source was rebuilt with the same corrected resource headers; the unmodified control reproduces the previous Main object. Startup and both graphical/direct launch routes passed verification. The clean binary has not received a separate full gameplay regression run.

Testing used AMD Radeon 8060S and Dreams CUSA04301 with version 2.64 content. Other hardware and versions remain unverified. The mip audit is still open: unavailable-feature sampling fallbacks, some LOD/derivative semantics, malformed layer ranges and multisample download handling still need work. Tren slowdowns and intermittent black patches/white-floor flashes also remain.

## Profile and tutorial

The launcher uses the normal `%APPDATA%\shadPS4` profile. It backs up a different per-game configuration before applying the included settings. It does not move portable saves or replace creations. The download contains no games, firmware, user saves or creations.

If a tutorial blocks progress, close Dreams and open **tutorial-skip/Skip Dreams Tutorial.exe**. Select your own `CUSA04301/SYSTEM/savedata.json`. The unchanged helper backs up that file before changing supported progression flags.

## Source and package

- [Checksums](../../builds/cusa04301-mip-fixes-20261026/SHA256SUMS.txt), [verification](../../builds/cusa04301-mip-fixes-20261026/verification.json) and [runtime file manifest](../../builds/cusa04301-mip-fixes-20261026/runtime-manifest.json).
- [Source manifest](../../builds/cusa04301-mip-fixes-20261026/source-manifest.json) and [rendering patch](../../patches/dreams-mip-rendering-20261026.patch). Apply the listed patches in order from the specified public base, including both preceding October 25 patches. The new delta changes 13 files; patch application and exported source hashes were verified. A fresh full-source rebuild is not claimed.
- [Launcher adapter source](../../builds/cusa04301-mip-fixes-20261026/DreamsCore.cs), [tutorial helper source](../../builds/cusa04301-mip-fixes-20261026/SkipDreamsTutorial.cs), and unchanged [Qt launcher source](../../builds/cusa04301-image-cache-20261021/qt-launcher-source-99e9829.tar.gz). Existing licenses and notices remain included.

All 45 ZIP members were allowlisted and hash-verified. The [previous shadow update](../../builds/cusa04301-shadow-fix-20261025/) remains available.
