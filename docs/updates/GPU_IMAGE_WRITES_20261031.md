# GPU image-write corrections — October 31, 2026

**[Download the graphical launcher package](https://github.com/JesusDaBest/Dreams-ShadPS4/raw/refs/heads/main/builds/cusa04301-gpu-image-writes-20261031/Dreams-graphical-launcher-gpu-image-writes-20261031.zip)**

Extract the ZIP, open **shadPS4QtLauncher.exe**, add Dreams and click **Play**.

This update includes all five tested corrections since the previous release:

- Complete GPU clears now invalidate cached image aliases wholly inside the written range.
- Complete GPU copies refresh those contained aliases instead of retaining stale pixels.
- GPU fills also update wholly written image aliases, while preserving the CPU fast path.
- Supported partial GPU fills update only the affected pixels and preserve the rest of the GPU-rendered image.
- Supported partial GPU copies use the same preservation rule, including aliases and safe fallback when an image becomes ineligible.

All [previous rendering, shadow, texture-cache and Tren fixes](MIP_READBACK_20261030.md) are retained.

## Verification

The clean release passed **4,296 GPU cases**: 2,520 copy, 1,560 fill and 216 clear cases, with zero mismatches or validation errors. The fixtures also retain 18 CPU fast-path checks, 21 partial-write guards and 93 clear guards. Before the corresponding corrections, the copy, fill and clear reproductions failed 924, 444 and 48 cases respectively.

The owner tested the combined private build and confirmed textures, shadows and movement look correct. The release retains its exact texture-cache object; the buffer and rasterizer objects were rebuilt with identical functional changes and without custom debug hooks. Unrelated linked objects were preserved. Both graphical and direct launcher routes were verified without starting another game.

Partial-write support covers texel-aligned, clean GPU-owned linear colour images in RGBA8, RGBA16Float, R32G32Uint and RGBA32Uint, including rows, mip levels, array layers and padding. Unsupported layouts and mixed dirty ownership retain the prior behavior; their correctness is not newly certified. Fixtures use production methods and native Vulkan operations with bounded allocation, registration and ownership adapters. The clean executable was not separately played, and other physical GPUs were not tested. No specific visible artifact or gameplay speedup is attributed to these fixes.

## Download and source

The graphical launcher, matching settings and optional tutorial-skip tool are included. No games, firmware, saves or creations are included.

Core SHA-256: `2cb99e6b42aeae3ef652dbd9539d6d7dc6c1b73f2b8c2ef1a172343a67630dc7`.

- [Checksums](../../builds/cusa04301-gpu-image-writes-20261031/SHA256SUMS.txt), [verification](../../builds/cusa04301-gpu-image-writes-20261031/verification.json), [runtime manifest](../../builds/cusa04301-gpu-image-writes-20261031/runtime-manifest.json).
- [Source manifest](../../builds/cusa04301-gpu-image-writes-20261031/source-manifest.json) and [new patch](../../patches/dreams-gpu-image-writes-20261031.patch). Apply every patch in order from the listed public base. All four changed files were reconstructed and verified; a fresh full-source rebuild is not claimed.
- [Launcher source](../../builds/cusa04301-gpu-image-writes-20261031/DreamsCore.cs), [tutorial helper](../../builds/cusa04301-gpu-image-writes-20261031/SkipDreamsTutorial.cs), unchanged [Qt launcher source](../../builds/cusa04301-image-cache-20261021/qt-launcher-source-99e9829.tar.gz).

The [previous package](../../builds/cusa04301-mip-readback-20261030/) remains available.
