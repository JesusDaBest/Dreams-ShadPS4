# Dynamic mip-write update — October 28, 2026

**[Download the graphical launcher package](https://github.com/JesusDaBest/Dreams-ShadPS4/raw/refs/heads/main/builds/cusa04301-dynamic-mip-20261028/Dreams-graphical-launcher-dynamic-mip-20261028.zip)**

Extract the ZIP, open **shadPS4QtLauncher.exe**, add Dreams and click **Play**.

This retains the [previous fixes](STORAGE_MULTISAMPLE_20261027.md) and corrects lost texture writes when GPU lanes select different mip levels in the fallback path. Each lane now writes to its selected mip. Only affected cached shaders are recompiled; ordinary image writes and native image-LOD support are unchanged. No resolution or detail reduction, new GPU feature requirement, or overall speedup is claimed.

Core SHA-256: `968d8b4d290c0094b67d8db8daee2532446eb3da56f5e8653af845c188153221`.

## Verification

- The old path produced 29 wrong texels in 10 native GPU cases. The correction passes 70 GPU cases checking 138,400 values, with zero mismatches or validation errors.
- 115 compiler cases pass. The original matrix has 10 expected changes and 44 byte-identical controls; 61 further fixtures cover descriptor ranges and the 16-entry bound.
- 72 cache checks pass. All 22 saved Dreams shaders remain byte-identical across four host profiles (88 comparisons).
- Two production source files and linked objects change; the parent objects were reproduced apart from COFF timestamps. The owner tested the exact executable in a familiar scene and reported correct textures, shadows and movement.

GPU testing used AMD Radeon 8060S and R32Uint 1D, 2D, 3D and array images, nonzero view bases, and out-of-view mip selections. Other physical GPUs and storage formats remain unverified. No matching live Dreams shader or FPS gain has been established, and a full persistent-cache warmup has not been replayed. The broader mip audit remains open.

## Download and source

The graphical launcher, matching settings and optional tutorial-skip tool are included. The package contains no games, firmware, saves or creations.

- [Checksums](../../builds/cusa04301-dynamic-mip-20261028/SHA256SUMS.txt), [verification](../../builds/cusa04301-dynamic-mip-20261028/verification.json), [runtime manifest](../../builds/cusa04301-dynamic-mip-20261028/runtime-manifest.json).
- [Source manifest](../../builds/cusa04301-dynamic-mip-20261028/source-manifest.json) and [new patch](../../patches/dreams-dynamic-mip-20261028.patch). Apply the listed patches in order from the public base. Patch application and source hashes are verified; a fresh full-source rebuild is not claimed.
- [Launcher adapter source](../../builds/cusa04301-dynamic-mip-20261028/DreamsCore.cs), [tutorial helper source](../../builds/cusa04301-dynamic-mip-20261028/SkipDreamsTutorial.cs), unchanged [Qt launcher source](../../builds/cusa04301-image-cache-20261021/qt-launcher-source-99e9829.tar.gz).

The 45 package files are allowlisted and hash-verified. Both launch routes were checked without starting another game. The [previous package](../../builds/cusa04301-storage-multisample-20261027/) remains available.
