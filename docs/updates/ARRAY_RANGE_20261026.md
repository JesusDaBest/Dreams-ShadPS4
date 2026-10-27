# Texture-array update — October 26, 2026

**[Download the graphical launcher package](https://github.com/JesusDaBest/Dreams-ShadPS4/raw/refs/heads/main/builds/cusa04301-array-range-20261026/Dreams-graphical-launcher-array-range-20261026.zip)**

Extract the ZIP, open **shadPS4QtLauncher.exe**, add Dreams and click **Play**.

This update retains all [previous fixes](MIP_FALLBACKS_20261026.md) and adds one texture-array bounds check. A descriptor pointing beyond its backing array now uses the existing null-texture path instead of requesting an invalid GPU image view. Valid array views retain their previous behavior. No FPS or memory-saving improvement is claimed.

Core SHA-256: `9abd5844a4feea8397450de8310a33d8aa4d761eadbfb0477809c77227d228fa`.

## Verification

- 917,760 descriptor cases; no out-of-range candidate views or changed valid controls.
- 256 GPU cases and 1,850 cache transitions; no incorrect results or Vulkan validation errors.
- All 22 saved Dreams shaders preserved across four capability combinations; all 96 affected compiled consumers checked.
- The exact executable completed a two-minute stationary 16,383-object test at 30 game updates/sec. This does not establish a full thermometer stress-test pass or visual regression pass.
- Tests used AMD Radeon 8060S. Other physical GPUs remain unverified. No matching invalid descriptor has been confirmed in a live Dreams frame; existing rendering glitches and performance issues remain.

## Download and source

The normal graphical launcher, matching settings and optional tutorial-skip tool are included. The package uses the normal shadPS4 profile and contains no games, firmware, saves or creations.

- [Checksums](../../builds/cusa04301-array-range-20261026/SHA256SUMS.txt), [verification](../../builds/cusa04301-array-range-20261026/verification.json), [runtime manifest](../../builds/cusa04301-array-range-20261026/runtime-manifest.json).
- [Source manifest](../../builds/cusa04301-array-range-20261026/source-manifest.json) and [new patch](../../patches/dreams-array-range-20261026.patch). Apply every listed patch in order from the stated public base. This delta changes one header; patch application and source hashes are verified. A fresh full-source rebuild is not claimed.
- [Launcher adapter source](../../builds/cusa04301-array-range-20261026/DreamsCore.cs), [tutorial helper source](../../builds/cusa04301-array-range-20261026/SkipDreamsTutorial.cs), unchanged [Qt launcher source](../../builds/cusa04301-image-cache-20261021/qt-launcher-source-99e9829.tar.gz).

All 45 package files are allowlisted and hash-verified. Both launch routes were verified without starting another game. The [previous package](../../builds/cusa04301-mip-fallbacks-20261026/) remains available. The broader mip audit remains open; this change does not cover 3D slices, non-array descriptors, reversed bounds with an in-range base, or physical mip-chain length.
