# Partial texture-write formats — October 31, 2026

**[Download the graphical launcher package](https://github.com/JesusDaBest/Dreams-ShadPS4/raw/refs/heads/main/builds/cusa04301-partial-image-formats-20261031/Dreams-graphical-launcher-partial-image-formats-20261031.zip)**

Extract the ZIP, open **shadPS4QtLauncher.exe**, add Dreams and click **Play**.

Partial GPU copies and fills could leave stale pixels in colour formats excluded from the earlier correction. This update extends that correction from four to **37 regular colour formats**, covering 8-, 16-, 32-, 64- and 128-bit texels while preserving pixels outside the written range. All [earlier rendering, shadow, texture-cache and Tren fixes](GPU_IMAGE_WRITES_20261031.md) are retained.

## Verification

The clean release passed **9,372 GPU cases**: 5,796 copy and 3,576 fill cases, with zero mismatches or validation errors. The previous code failed 3,014 of these cases across the 33 added formats. The fixtures also retain CPU fast-path, dirty-ownership, mip, array-layer, padding and bounds checks. The private candidate additionally passed all 4,080 earlier copy/fill regression cases unchanged.

The owner checked a familiar scene and confirmed textures, shadows and movement look correct. The release uses the exact tested texture-cache object; all other release objects are unchanged. Both graphical and direct launcher routes were verified without starting another game.

Support is limited to texel-aligned partial writes into valid GPU-owned linear colour images. Packed, compressed, depth, tiled and 96-bit formats, and mixed dirty ownership, remain outside this extension. Fixtures use production methods and native Vulkan operations with bounded allocation, registration and ownership adapters. The clean executable was not separately played, and other physical GPUs were not tested. No specific visible artifact or gameplay speedup is attributed to this fix.

## Download and source

The graphical launcher, matching settings and optional tutorial-skip tool are included. No games, firmware, saves or creations are included.

Core SHA-256: `34e65780b31266f1c6f9f4dcb2b930b387a6721d5852d4f75e66c008cb992f3c`.

- [Checksums](../../builds/cusa04301-partial-image-formats-20261031/SHA256SUMS.txt), [verification](../../builds/cusa04301-partial-image-formats-20261031/verification.json), [runtime manifest](../../builds/cusa04301-partial-image-formats-20261031/runtime-manifest.json).
- [Source manifest](../../builds/cusa04301-partial-image-formats-20261031/source-manifest.json) and [new patch](../../patches/dreams-partial-image-formats-20261031.patch). Apply every patch in order from the listed public base. The changed file was reconstructed and verified; a fresh full-source rebuild is not claimed.
- [Launcher source](../../builds/cusa04301-partial-image-formats-20261031/DreamsCore.cs), [tutorial helper](../../builds/cusa04301-partial-image-formats-20261031/SkipDreamsTutorial.cs), unchanged [Qt launcher source](../../builds/cusa04301-image-cache-20261021/qt-launcher-source-99e9829.tar.gz).

The [previous package](../../builds/cusa04301-gpu-image-writes-20261031/) remains available.
