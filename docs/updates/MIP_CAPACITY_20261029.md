# Mip allocation correction — October 29, 2026

**[Download the graphical launcher package](https://github.com/JesusDaBest/Dreams-ShadPS4/raw/refs/heads/main/builds/cusa04301-mip-capacity-20261029/Dreams-graphical-launcher-mip-capacity-20261029.zip)**

Extract the ZIP, open **shadPS4QtLauncher.exe**, add Dreams and click **Play**.

Texture-cache expansion could inherit more mip levels than the new image's dimensions support. For example, a valid 2×2 image could be expanded to three levels, although it can hold only two. The cache now bounds inherited levels to the destination dimensions while preserving requested levels, array layers and existing memory strides.

All [previous fixes](TREN_COLLISION_20261029.md), including the Tren disappearance correction, are retained.

Core SHA-256: `62757547a764a089e6757a313d10bdccbf831d3646e24196cf8a44c1a0bff1ab`.

## Verification

- All 48 previously invalid expansions pass native GPU image-creation and copy/readback checks with zero validation errors or data mismatches.
- 1,840 expansion checks pass. The 4,592 previously valid cache selections and 8,912 copy regions are unchanged.
- The private build passed an owner check of textures, shadows and movement in a familiar scene. Its executable, profile and settings were verified.
- The clean release retains the tested production objects and omits five diagnostic objects. Both graphical and direct launch routes passed verification without starting another game. All 45 package files are allowlisted and hash-verified.

The cache fixture checks production selection code; native tests cover its resulting image allocations and copies, not the complete cache lifecycle. The clean executable was not separately played. Testing used AMD Radeon 8060S; other physical GPUs remain untested. No specific live artifact or performance improvement is attributed to this fix.

## Download and source

The graphical launcher, matching settings and optional tutorial-skip tool are included. The package contains no games, firmware, saves or creations.

- [Checksums](../../builds/cusa04301-mip-capacity-20261029/SHA256SUMS.txt), [verification](../../builds/cusa04301-mip-capacity-20261029/verification.json), [runtime manifest](../../builds/cusa04301-mip-capacity-20261029/runtime-manifest.json).
- [Source manifest](../../builds/cusa04301-mip-capacity-20261029/source-manifest.json) and [new patch](../../patches/dreams-mip-capacity-20261029.patch). Apply every patch in the listed order from the public base. The changed header was reconstructed and verified; a fresh full-source rebuild is not claimed.
- [Launcher source](../../builds/cusa04301-mip-capacity-20261029/DreamsCore.cs), [tutorial helper](../../builds/cusa04301-mip-capacity-20261029/SkipDreamsTutorial.cs), unchanged [Qt launcher source](../../builds/cusa04301-image-cache-20261021/qt-launcher-source-99e9829.tar.gz).

The [previous package](../../builds/cusa04301-tren-collision-20261029/) remains available.
