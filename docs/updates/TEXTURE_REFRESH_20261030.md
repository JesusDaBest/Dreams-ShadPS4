# Texture-refresh correction — October 30, 2026

**[Download the graphical launcher package](https://github.com/JesusDaBest/Dreams-ShadPS4/raw/refs/heads/main/builds/cusa04301-texture-refresh-20261030/Dreams-graphical-launcher-texture-refresh-20261030.zip)**

Extract the ZIP, open **shadPS4QtLauncher.exe**, add Dreams and click **Play**.

Small textures sharing memory pages could be refreshed unnecessarily or keep stale pixels after changes beyond the start of the texture. The cache now compares the same complete memory range when checking for changes. A known GPU buffer update also takes priority over the CPU-only unchanged check, so it reaches the first texture lookup.

All [previous rendering, shadow, texture-cache and Tren fixes](SAMPLER_BORDER_20261030.md) are retained.

## Verification

- All 168 GPU cases passed with no stale-pixel failures or validation errors. The preceding code failed 48 cases, including 24 GPU updates delayed until the next lookup. The correction also eliminated 168 unnecessary uploads in these tests.
- Checks cover linear RGBA8, BC1 and BC3 textures, one/three mip levels, one/two layers, unchanged adjacent accesses, changes in later data, and definite CPU/GPU update controls.
- The owner confirmed textures, shadows and movement still look correct in the private build.
- The clean release retains the tested production objects and removes five diagnostic objects. Both launch routes and all 45 package files are verified. The clean executable was not separately played.

Tests execute the production cache methods and native GPU image uploads/readbacks. Page ownership and source-buffer delivery use bounded fixtures; operating-system page faults, detiling and multisample behavior were not newly tested. No particular visible Dreams artifact or frame-rate improvement is attributed to this fix, and other physical GPUs remain untested.

## Download and source

The graphical launcher, matching settings and optional tutorial-skip tool are included. No games, firmware, saves or creations are included.

Core SHA-256: `4d229cf75a108bf8f9c6cb44162cfc75d8421e400b5805e290cea60bd9b712d4`.

- [Checksums](../../builds/cusa04301-texture-refresh-20261030/SHA256SUMS.txt), [verification](../../builds/cusa04301-texture-refresh-20261030/verification.json), [runtime manifest](../../builds/cusa04301-texture-refresh-20261030/runtime-manifest.json).
- [Source manifest](../../builds/cusa04301-texture-refresh-20261030/source-manifest.json) and [new patch](../../patches/dreams-texture-refresh-20261030.patch). Apply every patch in the listed order from the public base. The changed source was reconstructed and verified; a fresh full-source rebuild is not claimed.
- [Launcher source](../../builds/cusa04301-texture-refresh-20261030/DreamsCore.cs), [tutorial helper](../../builds/cusa04301-texture-refresh-20261030/SkipDreamsTutorial.cs), unchanged [Qt launcher source](../../builds/cusa04301-image-cache-20261021/qt-launcher-source-99e9829.tar.gz).

The [previous package](../../builds/cusa04301-sampler-border-20261030/) remains available.
