# Sampling compatibility update — October 26, 2026

**[Download the graphical launcher package](https://github.com/JesusDaBest/Dreams-ShadPS4/raw/refs/heads/main/builds/cusa04301-mip-fallbacks-20261026/Dreams-graphical-launcher-mip-fallbacks-20261026.zip)**

Extract the complete ZIP, open **shadPS4QtLauncher.exe**, add your own Dreams game folder and click **Play**. The core and settings apply automatically. The optional tutorial-skip tool is retained.

## Changes

This update retains all fixes from the [earlier October 26 build](MIP_RENDERING_20261026.md) and adds three bounded corrections for drivers missing the corresponding Vulkan features:

- Use the selected mip's dimensions for supported explicit-LOD, nearest-mip runtime texture offsets.
- Respect a common minimum image mip through sampler state when native image-view minimum-LOD support is absent.
- Return zero for supported 2D/2D-array image fetches and ordinary gathers excluded by the image-view minimum, and keep shader-cache entries distinct when that minimum changes.

Feature support is detected at runtime; this is not a GPU-brand switch. Supported native paths remain in use. These are correctness fixes, with no general FPS or memory-saving improvement established.

Core SHA-256: `6030c3971105be9e44454fdfb22deb803531beba263c1fcaafbdfe0838efda7e`.

## Validation and limits

- Offset correction: 240 supported GPU cases and 315 cache/guard checks pass. Another 88 excluded cases retain their old code; they are not counted as repaired. 22 saved shaders were preserved across two capability configurations.
- Sampler correction: 2,304 GPU cases, 2,007 guard checks and 216 production sampler-cache checks pass.
- Image-read correction: 576 actual-instruction GPU cases, 2,367 cache/guard checks and 576 native shader comparisons pass. All 22 saved Dreams shaders remain byte-identical across four capability configurations (88 comparisons). All 96 affected header consumers were reviewed; controls for changed objects reproduce the previous executable's objects.
- Relevant GPU tests reported zero validation errors. Tests used AMD Radeon 8060S; unavailable-feature routes were forced locally. No other physical GPU was tested.
- The preceding build containing the first two fixes passed a live Tren check, including camera movement, with correct shadows, textures, objects and movement. The final executable adds the third fix and has passed offline tests and startup; it has not received a separate full gameplay test. There is no controlled performance comparison.

The offset repair is restricted to the tested nearest-mip, non-anisotropic, non-cube, zero-image-minimum case, with sampler bias from -2 to 0 and excluding a fixed half-mip clamp. Positive bias, implicit/derivative and linear-mip fallbacks remain open. The sampler repair requires a common effective view minimum and equal non-anisotropic spatial filters. Mixed minima and incompatible filters remain open. The new image-read repair excludes depth-comparison gathers, cube, 1D, 3D, multisample and storage-image minimum handling. GPU witnesses used R32 float/uint as documented by their test scope; other formats are not independently covered here. Cache tests use bounded fixtures rather than a complete guest-command replay.

Tren slowdowns and intermittent black patches/white flashes remain. Testing has focused on Dreams CUSA04301 version 2.64. The broader mip audit is still open.

## Download and source

The package uses the normal `%APPDATA%\shadPS4` profile. It contains no games, firmware, user saves or creations. Different per-game settings are backed up before applying the included configuration; portable profiles are not moved automatically.

- [Checksums](../../builds/cusa04301-mip-fallbacks-20261026/SHA256SUMS.txt), [verification](../../builds/cusa04301-mip-fallbacks-20261026/verification.json) and [runtime manifest](../../builds/cusa04301-mip-fallbacks-20261026/runtime-manifest.json).
- [Source manifest](../../builds/cusa04301-mip-fallbacks-20261026/source-manifest.json) and [new patch](../../patches/dreams-mip-fallbacks-20261026.patch). Apply every listed patch in order from the stated public base. The new delta changes 10 files; patch application and exported source hashes were verified. A fresh full-source rebuild is not claimed.
- [Launcher adapter source](../../builds/cusa04301-mip-fallbacks-20261026/DreamsCore.cs), [tutorial helper source](../../builds/cusa04301-mip-fallbacks-20261026/SkipDreamsTutorial.cs) and unchanged [Qt launcher source](../../builds/cusa04301-image-cache-20261021/qt-launcher-source-99e9829.tar.gz). Licenses and notices remain included.

All 45 ZIP members were allowlisted and hash-verified. Both the graphical launcher and direct launch route passed configuration-only verification without starting another game or modifying the current profile. The [previous package](../../builds/cusa04301-mip-fixes-20261026/) remains available.
