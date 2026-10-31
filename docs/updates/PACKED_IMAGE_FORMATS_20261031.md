# Packed texture-write formats - October 31, 2026

**[Download the graphical launcher package](https://github.com/JesusDaBest/Dreams-ShadPS4/raw/refs/heads/main/builds/cusa04301-packed-image-formats-20261031/Dreams-graphical-launcher-packed-image-formats-20261031.zip)**

Extract the ZIP, open **shadPS4QtLauncher.exe**, add Dreams and click **Play**.

Partial GPU copies and fills could leave stale pixels in packed colour formats excluded from the earlier correction. This update adds **eight packed 16- and 32-bit formats**, alongside the 37 regular formats already supported, while preserving pixels outside the written range. All [earlier rendering, shadow, texture-cache and Tren fixes](PARTIAL_IMAGE_FORMATS_20261031.md) are retained.

## Verification

The clean release passed **2,064 GPU cases**: 1,272 copy and 792 fill cases, with zero mismatches or validation errors. The previous code failed 760 of these cases across the eight added formats. The fixtures also retain CPU fast-path, dirty-ownership, mip, array-layer, padding and bounds checks. The private candidate additionally passed all 9,372 earlier regular-format copy/fill regression cases unchanged.

The owner checked a familiar scene and confirmed textures, shadows and movement look correct. The release uses the exact tested texture-cache object; all other release objects are unchanged. Both graphical and direct launcher routes were verified without starting another game.

Support is limited to texel-aligned partial writes into valid GPU-owned linear colour images. Signed 10:10:10:2, compressed, depth, tiled and 96-bit formats, and mixed dirty ownership, remain outside this extension. Fixtures use production methods and native Vulkan operations with bounded allocation, registration and ownership adapters. The clean executable was not separately played, and other physical GPUs were not tested. No specific visible artifact or gameplay speedup is attributed to this fix.

## Download and source

The graphical launcher, matching settings and optional tutorial-skip tool are included. No games, firmware, saves or creations are included.

Core SHA-256: `a4f15cb744ebffaaebbfc481e51667050cafc741bc6474fab04355e5c3a09946`.

- [Checksums](../../builds/cusa04301-packed-image-formats-20261031/SHA256SUMS.txt), [verification](../../builds/cusa04301-packed-image-formats-20261031/verification.json), [runtime manifest](../../builds/cusa04301-packed-image-formats-20261031/runtime-manifest.json).
- [Source manifest](../../builds/cusa04301-packed-image-formats-20261031/source-manifest.json) and [new patch](../../patches/dreams-packed-image-formats-20261031.patch). Apply every patch in order from the listed public base. The changed file was reconstructed and verified; a fresh full-source rebuild is not claimed.
- [Launcher source](../../builds/cusa04301-packed-image-formats-20261031/DreamsCore.cs), [tutorial helper](../../builds/cusa04301-packed-image-formats-20261031/SkipDreamsTutorial.cs), unchanged [Qt launcher source](../../builds/cusa04301-image-cache-20261021/qt-launcher-source-99e9829.tar.gz).

The [previous package](../../builds/cusa04301-partial-image-formats-20261031/) remains available.
