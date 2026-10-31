# Mip-refresh correction — October 30, 2026

**[Download the graphical launcher package](https://github.com/JesusDaBest/Dreams-ShadPS4/raw/refs/heads/main/builds/cusa04301-mip-refresh-20261030/Dreams-graphical-launcher-mip-refresh-20261030.zip)**

Extract the ZIP, open **shadPS4QtLauncher.exe**, add Dreams and click **Play**.

Updating one texture mip could overwrite other mip levels that still held GPU-rendered pixels. The cache now records the CPU baseline when a texture becomes GPU-owned and maintains it across refreshes, copies and resolves. Pending CPU edits are refreshed before partial writes.

All [previous rendering, shadow, texture-cache and Tren fixes](TEXTURE_REFRESH_20261030.md) are retained.

## Verification

- All 336 GPU cases passed with no pixel mismatches or validation errors, including a repeat against the clean release's production link. The original 96-case reproduction failed 48 cases before the fix.
- Checks cover linear RGBA8, BC1 and BC3 textures, two/three mip levels, one/two layers, GPU writes, copies, partial resolves, clears, pending CPU edits and allocation growth. Another 2.4 million clean-bind checks confirmed no redundant uploads or snapshot changes.
- The owner confirmed textures, shadows and movement look correct in the private build. The clean rasterizer was rebuilt with exactly the same rendering changes and no custom debug hooks; unchanged production objects were retained. Both launcher routes and the package contents were verified.

The clean executable was not separately played. Fixtures use production image operations and extracted refresh methods with bounded source-buffer and page-ownership adapters. OS page faults, detiling, multisample behavior and other physical GPUs were not newly tested. This fix is not attributed to a particular visible artifact or level-unloading behavior; no gameplay speedup is claimed.

## Download and source

The graphical launcher, matching settings and optional tutorial-skip tool are included. No games, firmware, saves or creations are included.

Core SHA-256: `5172dcbcd834369c4ed09d23bd78eed24b607bd9dfbd3e63e0c8376423ebbc59`.

- [Checksums](../../builds/cusa04301-mip-refresh-20261030/SHA256SUMS.txt), [verification](../../builds/cusa04301-mip-refresh-20261030/verification.json), [runtime manifest](../../builds/cusa04301-mip-refresh-20261030/runtime-manifest.json).
- [Source manifest](../../builds/cusa04301-mip-refresh-20261030/source-manifest.json) and [new patch](../../patches/dreams-mip-refresh-20261030.patch). Apply every patch in order from the listed public base. All four changed source files were reconstructed and verified; a fresh full-source rebuild is not claimed.
- [Launcher source](../../builds/cusa04301-mip-refresh-20261030/DreamsCore.cs), [tutorial helper](../../builds/cusa04301-mip-refresh-20261030/SkipDreamsTutorial.cs), unchanged [Qt launcher source](../../builds/cusa04301-image-cache-20261021/qt-launcher-source-99e9829.tar.gz).

The [previous package](../../builds/cusa04301-texture-refresh-20261030/) remains available.
