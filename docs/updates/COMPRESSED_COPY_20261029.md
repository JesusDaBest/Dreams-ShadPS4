# Compressed texture copy correction — October 29, 2026

**[Download the graphical launcher package](https://github.com/JesusDaBest/Dreams-ShadPS4/raw/refs/heads/main/builds/cusa04301-compressed-copy-20261029/Dreams-graphical-launcher-compressed-copy-20261029.zip)**

Extract the ZIP, open **shadPS4QtLauncher.exe**, add Dreams and click **Play**.

This retains the [previous fixes](DISPLAY_COLOUR_20261028.md), including corrected display colours, and repairs cache-growth copies between compressed textures whose visible dimensions differ. The old copy could exceed the smaller image or use an invalid partial-block extent. The corrected path transfers the shared blocks through a buffer, allowing each image to use its own valid extent while preserving unaffected mip levels and slices. Ordinary copies retain their existing path.

Core SHA-256: `79aa37eb560500fbec19ae209ce0d9d64eee82d1f3dd76d119e9f057608da4fd`.

## Verification

- The production cache-selection test exercises 2,160 requests and reaches 864 copies. The original code produces 928 out-of-bounds regions and 1,328 incompatible block-edge regions; those categories overlap. The corrected region checks pass, including 408 mocked oversized-buffer fallbacks.
- The exact candidate passes 2,952 GPU cases covering compressed edges, mip levels, array slices and volume depths, plus 768 depth/colour regression cases. All 3,720 cases have byte-exact results and zero validation errors. A further 552 legal controls pass on the previous build.
- The owner checked a familiar scene with this exact executable and confirmed correct textures, shadows and movement. The repaired branch was not independently observed in that scene, so this is a visual regression check rather than proof of a particular in-game artifact being fixed.
- Two production source files and two linked objects change. Prior rendering and colour corrections are retained. No performance improvement is claimed; the intermediate transfer is limited to compressed cache growth with different visible dimensions.

GPU tests used AMD Radeon 8060S. Other physical GPUs were not tested. Compressed test uploads complete in a separate submission to avoid a reproduced validation-layer range-accounting error at partial block edges; the production copy and readbacks retain synchronization validation together. Oversized allocation routing and deferred lifetime were checked with mocks, not a large native GPU allocation. Mixed compressed/uncompressed and stencil transfer coverage is not claimed.

## Download and source

The graphical launcher, matching settings and optional tutorial-skip tool are included. The package contains no games, firmware, saves or creations.

- [Checksums](../../builds/cusa04301-compressed-copy-20261029/SHA256SUMS.txt), [verification](../../builds/cusa04301-compressed-copy-20261029/verification.json), [runtime manifest](../../builds/cusa04301-compressed-copy-20261029/runtime-manifest.json).
- [Source manifest](../../builds/cusa04301-compressed-copy-20261029/source-manifest.json) and [new patch](../../patches/dreams-compressed-copy-20261029.patch). Apply the listed patches in order from the public base. Both changed files were reconstructed and verified against that stack; a fresh full-source rebuild is not claimed.
- [Launcher adapter source](../../builds/cusa04301-compressed-copy-20261029/DreamsCore.cs), [tutorial helper source](../../builds/cusa04301-compressed-copy-20261029/SkipDreamsTutorial.cs), unchanged [Qt launcher source](../../builds/cusa04301-image-cache-20261021/qt-launcher-source-99e9829.tar.gz).

The 45 package files are allowlisted and hash-verified. Both launch routes were checked without starting another game. The [previous package](../../builds/cusa04301-display-colour-20261028/) remains available.
