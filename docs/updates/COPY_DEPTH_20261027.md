# 3D texture-copy update — October 27, 2026

**[Download the graphical launcher package](https://github.com/JesusDaBest/Dreams-ShadPS4/raw/refs/heads/main/builds/cusa04301-copy-depth-20261027/Dreams-graphical-launcher-copy-depth-20261027.zip)**

Extract the ZIP, open **shadPS4QtLauncher.exe**, add Dreams and click **Play**.

This keeps the [previous fixes](MIP_DEPTH_20261027.md) and corrects one 3D texture-copy bug. When cached images grow, padded allocations can have equal byte sizes but different depths. The old copy could read four slices from a three-slice source. The corrected copy uses only the depth shared by both images and preserves the destination's remaining data.

Resolution and detail are unchanged. No overall speedup is claimed.

Core SHA-256: `b6e03657c1f99ad7167d5bace1b99225da659f2a3e22e1764fd4e4586924eb1a`.

## Verification

- 162 array-growth cases and 378 same-address volume cases: 171 out-of-bounds copy regions before the change, zero after it.
- 296 native GPU cases verify copied pixels, unchanged source images, preserved extra slices and mip levels, and readback guards. No Vulkan validation errors.
- 232 legal GPU controls pass with the preceding executable's copy code. Invalid parent commands were checked on the CPU, not submitted to the GPU.
- One source file and linked object change; an exact parent-object control was verified. All other compiled objects retain the previous fixes.
- The release uses the same executable and settings as the local test session.

Tests used AMD Radeon 8060S. Other physical GPUs remain unverified. The automated tests establish this copy-bounds defect; they do not attribute a particular live Dreams glitch to it or establish an FPS benefit. The broader texture audit continues.

## Download and source

The graphical launcher, matching settings and optional tutorial-skip tool are included. The package contains no games, firmware, saves or creations.

- [Checksums](../../builds/cusa04301-copy-depth-20261027/SHA256SUMS.txt), [verification](../../builds/cusa04301-copy-depth-20261027/verification.json), [runtime manifest](../../builds/cusa04301-copy-depth-20261027/runtime-manifest.json).
- [Source manifest](../../builds/cusa04301-copy-depth-20261027/source-manifest.json) and [new patch](../../patches/dreams-copy-depth-20261027.patch). Apply the listed patches in order from the public base. Patch application and source hashes are verified; a fresh full-source rebuild is not claimed.
- [Launcher adapter source](../../builds/cusa04301-copy-depth-20261027/DreamsCore.cs), [tutorial helper source](../../builds/cusa04301-copy-depth-20261027/SkipDreamsTutorial.cs), unchanged [Qt launcher source](../../builds/cusa04301-image-cache-20261021/qt-launcher-source-99e9829.tar.gz).

The 45 package files are allowlisted and hash-verified. Both launch routes were checked without starting another game. The [previous package](../../builds/cusa04301-mip-depth-20261027/) remains available.
