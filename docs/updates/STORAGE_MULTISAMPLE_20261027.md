# Multisampled texture-write update — October 27, 2026

**[Download the graphical launcher package](https://github.com/JesusDaBest/Dreams-ShadPS4/raw/refs/heads/main/builds/cusa04301-storage-multisample-20261027/Dreams-graphical-launcher-storage-multisample-20261027.zip)**

Extract the ZIP, open **shadPS4QtLauncher.exe**, add Dreams and click **Play**.

This retains the [previous fixes](COPY_RESOLVE_20261027.md) and corrects a missing shader declaration for writes to multisampled textures, including texture arrays. Older cached shaders missing that declaration are recompiled selectively; other cached shaders are retained. Resolution and detail settings are unchanged. No overall speedup is claimed.

Core SHA-256: `9902e3b8283f4b5bdc1ec00a92c8d841ecd0cc9b9378913168bcf00224103f9f`.

## Verification

- 54 native GCN compiler cases: all pass, including four previously invalid shaders; the other 50 remain byte-identical.
- 12 native GPU cases check 147,456 sample values plus untouched guards, with zero mismatches or validation errors. They cover R32Uint 2D/array images, four samples, constant/varying sample indices and nonzero view layers.
- 69 checks cover selective cache acceptance and rejection using the extracted production function. All 22 saved Dreams shaders remain byte-identical across four host profiles (88 comparisons).
- Two source files and linked objects change. Parent compilation controls reproduce exactly apart from COFF timestamps; other compiled objects retain all preceding fixes.
- The owner checked this exact executable in a familiar scene and reported correct textures, shadows and movement.

GPU testing used AMD Radeon 8060S. Other physical GPUs remain unverified. No matching live Dreams shader or visible symptom has been attributed to this defect. Tests do not cover other storage formats, atomics, or a full persistent-cache reload. Dynamic mip descriptor indexing and the wider mip audit remain open.

## Download and source

The graphical launcher, matching settings and optional tutorial-skip tool are included. The package contains no games, firmware, saves or creations.

- [Checksums](../../builds/cusa04301-storage-multisample-20261027/SHA256SUMS.txt), [verification](../../builds/cusa04301-storage-multisample-20261027/verification.json), [runtime manifest](../../builds/cusa04301-storage-multisample-20261027/runtime-manifest.json).
- [Source manifest](../../builds/cusa04301-storage-multisample-20261027/source-manifest.json) and [new patch](../../patches/dreams-storage-multisample-20261027.patch). Apply the listed patches in order from the public base. Patch application and source hashes are verified; a fresh full-source rebuild is not claimed.
- [Launcher adapter source](../../builds/cusa04301-storage-multisample-20261027/DreamsCore.cs), [tutorial helper source](../../builds/cusa04301-storage-multisample-20261027/SkipDreamsTutorial.cs), unchanged [Qt launcher source](../../builds/cusa04301-image-cache-20261021/qt-launcher-source-99e9829.tar.gz).

The 45 package files are allowlisted and hash-verified. Both launch routes were checked without starting another game. The [previous package](../../builds/cusa04301-copy-resolve-20261027/) remains available.
