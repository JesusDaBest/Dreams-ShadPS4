# Depth texture correction — October 29, 2026

**[Download the graphical launcher package](https://github.com/JesusDaBest/Dreams-ShadPS4/raw/refs/heads/main/builds/cusa04301-depth-growth-20261029/Dreams-graphical-launcher-depth-growth-20261029.zip)**

Extract the ZIP, open **shadPS4QtLauncher.exe**, add Dreams and click **Play**.

When the texture cache replaced a depth image with a larger image, only the old image's contents were copied. Newly exposed mip levels, layers or texels could remain uninitialized. The replacement now loads those contents from guest memory before copying the cached GPU data over the overlapping region. Fully covered conversions keep their existing path without an extra upload.

All [previous fixes](MIP_CAPACITY_20261029.md), including the Tren disappearance correction, are retained.

Core SHA-256: `9fd42e97ce000ab72525d01332c473e87ef41600c0fd703fe16246d45d1000a3`.

## Verification

- 384 native GPU cases pass with zero validation errors or mismatches, checking 1,570,816 bytes. The previous code left data uninitialized in 320 of these cases.
- 384 production-code checks pass; the 64 fully covered conversions retain their original no-upload path.
- Coverage includes single-sample D32/R32 conversions in both directions, direct and buffer-mediated copies, mip chains, arrays, different widths, and linear and tiled descriptors. Existing GPU contents and scratch-buffer guards are preserved.
- The private build passed the owner's check of textures, shadows and movement in a familiar scene. Its executable, profile and settings were verified.
- The clean release retains the tested production objects and omits five diagnostic objects. Both graphical and direct launch routes passed verification without starting another game. All 45 package files are allowlisted and hash-verified.

The GPU replay uses the production depth-overlap body and image allocation, upload and copy code. Guest-byte acquisition, detiling and cache ownership use bounded test fixtures. New allocations are poisoned only by the fixture to expose missing initialization. Multisample and stencil behavior are unchanged. The clean executable was not separately played; testing used AMD Radeon 8060S. No matching live artifact or performance gain is claimed.

## Download and source

The graphical launcher, matching settings and optional tutorial-skip tool are included. The package contains no games, firmware, saves or creations.

- [Checksums](../../builds/cusa04301-depth-growth-20261029/SHA256SUMS.txt), [verification](../../builds/cusa04301-depth-growth-20261029/verification.json), [runtime manifest](../../builds/cusa04301-depth-growth-20261029/runtime-manifest.json).
- [Source manifest](../../builds/cusa04301-depth-growth-20261029/source-manifest.json) and [new patch](../../patches/dreams-depth-growth-20261029.patch). Apply every patch in the listed order from the public base. The changed source file was reconstructed and verified; a fresh full-source rebuild is not claimed.
- [Launcher source](../../builds/cusa04301-depth-growth-20261029/DreamsCore.cs), [tutorial helper](../../builds/cusa04301-depth-growth-20261029/SkipDreamsTutorial.cs), unchanged [Qt launcher source](../../builds/cusa04301-image-cache-20261021/qt-launcher-source-99e9829.tar.gz).

The [previous package](../../builds/cusa04301-mip-capacity-20261029/) remains available.
