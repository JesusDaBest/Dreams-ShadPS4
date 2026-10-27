# Array and volume texture-copy update — October 27, 2026

**[Download the graphical launcher package](https://github.com/JesusDaBest/Dreams-ShadPS4/raw/refs/heads/main/builds/cusa04301-mixed-copy-20261027/Dreams-graphical-launcher-mixed-copy-20261027.zip)**

Extract the ZIP, open **shadPS4QtLauncher.exe**, add Dreams and click **Play**.

This keeps the [previous fixes](COPY_DEPTH_20261027.md) and corrects copies between texture arrays and 3D textures. Equal-sized memory allocations do not always contain the same number of layers or depth slices. The old code could copy five slices into an array with only four layers, or request source layers that do not exist. The correction limits each copy to the common extent and preserves the destination's remaining data.

Resolution and detail are unchanged. No overall speedup is claimed.

Core SHA-256: `f820c9d0da42de1ed101508cefa377ff60c1703631cc1296b5b88e80f06dd428`.

## Verification

- 892 reachable CPU cases covering 1,266 copy regions: 700 invalid regions before the change, zero after it. All 566 legal regions retain their previous behavior.
- 552 native GPU cases verify copied pixels, unchanged source images, preserved extra slices and mip levels, and readback guards. No Vulkan validation errors. These include the preceding 296 copy-depth regression cases.
- 424 legal GPU controls pass with the preceding executable's copy code. Invalid parent commands were checked on the CPU, not submitted to the GPU.
- One source file and linked object change; an exact parent-object control was verified. All other compiled objects retain the previous fixes.
- The release uses the same executable and settings as the local test session.

Tests used AMD Radeon 8060S. Other physical GPUs remain unverified. The automated tests establish this copy-bounds defect; they do not attribute a particular live Dreams glitch to it or establish an FPS benefit. The mixed-copy GPU tests cover RGBA8 and R32 images; compressed/depth formats and the complete guest-memory refresh path were not independently replayed. The broader texture audit continues.

## Download and source

The graphical launcher, matching settings and optional tutorial-skip tool are included. The package contains no games, firmware, saves or creations.

- [Checksums](../../builds/cusa04301-mixed-copy-20261027/SHA256SUMS.txt), [verification](../../builds/cusa04301-mixed-copy-20261027/verification.json), [runtime manifest](../../builds/cusa04301-mixed-copy-20261027/runtime-manifest.json).
- [Source manifest](../../builds/cusa04301-mixed-copy-20261027/source-manifest.json) and [new patch](../../patches/dreams-mixed-copy-20261027.patch). Apply the listed patches in order from the public base. Patch application and source hashes are verified; a fresh full-source rebuild is not claimed.
- [Launcher adapter source](../../builds/cusa04301-mixed-copy-20261027/DreamsCore.cs), [tutorial helper source](../../builds/cusa04301-mixed-copy-20261027/SkipDreamsTutorial.cs), unchanged [Qt launcher source](../../builds/cusa04301-image-cache-20261021/qt-launcher-source-99e9829.tar.gz).

The 45 package files are allowlisted and hash-verified. Both launch routes were checked without starting another game. The [previous package](../../builds/cusa04301-copy-depth-20261027/) remains available.
