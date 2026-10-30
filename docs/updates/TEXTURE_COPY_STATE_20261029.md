# Texture-copy cache correction — October 29, 2026

**[Download the graphical launcher package](https://github.com/JesusDaBest/Dreams-ShadPS4/raw/refs/heads/main/builds/cusa04301-texture-copy-state-20261029/Dreams-graphical-launcher-texture-copy-state-20261029.zip)**

Extract the ZIP, open **shadPS4QtLauncher.exe**, add Dreams and click **Play**.

Some successful texture copies preserved the pixels but lost the flag identifying them as GPU-modified. Later reads and cache cleanup could then ignore valid GPU contents. The cache now preserves that flag after copying a clean GPU-owned image during backing expansion, depth conversion or mip merging. CPU-only sources and copies that do not execute keep their existing behavior.

All [previous fixes](DEPTH_COPY_BUFFER_20261029.md), including bounded depth-copy buffers, shadow repairs and the Tren disappearance correction, are retained.

## Verification

- 192 native GPU cases passed with zero pixel mismatches or validation errors, checking 5,753,856 bytes. The preceding code lost ownership in 80 cases; the correction lost it in none. Copied pixels and untouched mip levels/layers were preserved.
- 288 production-code cases, 22 negative controls and 12 multisample-blit state checks passed. The preceding code lost ownership in 126 ordinary cases and 3 blit-state cases; the correction lost it in none.
- The owner confirmed textures, shadows and movement still look correct in the private build.
- The clean release retains the tested production objects and removes five diagnostic objects. Both launch routes and all 45 package files are verified. The clean executable was not separately played.

Testing used AMD Radeon 8060S. The GPU replay uses production cache methods and real image copy/download operations, with bounded allocation and guest-upload fixtures. GPU coverage includes single-sample depth/colour conversion, compressed and uncompressed growth, and nonzero mip/layer merging. Multisample ownership was checked on CPU only. Dirty-source and unsupported paths remain unchanged. No specific visible artifact or speedup is attributed to this fix; other physical GPUs remain untested.

## Download and source

The graphical launcher, matching settings and optional tutorial-skip tool are included. No games, firmware, saves or creations are included.

Core SHA-256: `f332df8f1d8f2cc1f1add599482b3341cc240f7335554e02ce255e8c8a1183d1`.

- [Checksums](../../builds/cusa04301-texture-copy-state-20261029/SHA256SUMS.txt), [verification](../../builds/cusa04301-texture-copy-state-20261029/verification.json), [runtime manifest](../../builds/cusa04301-texture-copy-state-20261029/runtime-manifest.json).
- [Source manifest](../../builds/cusa04301-texture-copy-state-20261029/source-manifest.json) and [new patch](../../patches/dreams-texture-copy-state-20261029.patch). Apply every patch in the listed order from the public base. The changed source file was reconstructed and verified; a fresh full-source rebuild is not claimed.
- [Launcher source](../../builds/cusa04301-texture-copy-state-20261029/DreamsCore.cs), [tutorial helper](../../builds/cusa04301-texture-copy-state-20261029/SkipDreamsTutorial.cs), unchanged [Qt launcher source](../../builds/cusa04301-image-cache-20261021/qt-launcher-source-99e9829.tar.gz).

The [previous package](../../builds/cusa04301-depth-copy-buffer-20261029/) remains available.
