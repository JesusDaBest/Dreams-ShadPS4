# Texture-copy width update — October 27, 2026

**[Download the graphical launcher package](https://github.com/JesusDaBest/Dreams-ShadPS4/raw/refs/heads/main/builds/cusa04301-copy-width-20261027/Dreams-graphical-launcher-copy-width-20261027.zip)**

Extract the ZIP, open **shadPS4QtLauncher.exe**, add Dreams and click **Play**.

This keeps the [previous fixes](MIXED_COPY_20261027.md) and corrects uncompressed texture copies into narrower images. Two textures can share the same memory pitch while having different visible widths. The old copy used the source width even when it extended past the destination. Each mip now copies only the shared visible width, preserving the destination's remaining data. Compressed-copy handling is unchanged.

Resolution and detail are unchanged. No overall speedup is claimed.

Core SHA-256: `8cfc8f11c943dc17aa7c29b1d32a1d60e7c69a620151b5f116f10d3ed9d8bc1a`.

## Verification

- 3,262 reachable CPU cases covering 5,530 copy regions: 2,046 invalid widths before the change, zero after it. All 3,484 legal regions retain their previous behavior. The existing cache check also rejects all 1,799 mismatched-height inputs tested.
- 936 native GPU cases verify copied pixels row by row, unchanged source images, preserved destination pixels and mip levels, and readback guards. No Vulkan validation errors. These include all 552 preceding array/volume regression cases.
- 744 legal GPU controls pass with the preceding executable's copy code. Invalid parent commands were checked on the CPU, not submitted to the GPU.
- One source file and linked object change; an exact parent-object control was verified. All other compiled objects retain the previous fixes.
- The release uses the same executable and settings as the local test session.

Tests used AMD Radeon 8060S. Other physical GPUs remain unverified. The automated tests establish this copy-bounds defect; they do not attribute a particular live Dreams glitch to it or establish an FPS benefit. The width-copy GPU tests cover RGBA8 and R32 images across all four 2D-array/3D source and destination combinations; compressed/depth formats and the complete guest-memory refresh path were not independently replayed. The broader texture audit continues.

## Download and source

The graphical launcher, matching settings and optional tutorial-skip tool are included. The package contains no games, firmware, saves or creations.

- [Checksums](../../builds/cusa04301-copy-width-20261027/SHA256SUMS.txt), [verification](../../builds/cusa04301-copy-width-20261027/verification.json), [runtime manifest](../../builds/cusa04301-copy-width-20261027/runtime-manifest.json).
- [Source manifest](../../builds/cusa04301-copy-width-20261027/source-manifest.json) and [new patch](../../patches/dreams-copy-width-20261027.patch). Apply the listed patches in order from the public base. Patch application and source hashes are verified; a fresh full-source rebuild is not claimed.
- [Launcher adapter source](../../builds/cusa04301-copy-width-20261027/DreamsCore.cs), [tutorial helper source](../../builds/cusa04301-copy-width-20261027/SkipDreamsTutorial.cs), unchanged [Qt launcher source](../../builds/cusa04301-image-cache-20261021/qt-launcher-source-99e9829.tar.gz).

The 45 package files are allowlisted and hash-verified. Both launch routes were checked without starting another game. The [previous package](../../builds/cusa04301-mixed-copy-20261027/) remains available.
