# Depth-copy buffer correction — October 29, 2026

**[Download the graphical launcher package](https://github.com/JesusDaBest/Dreams-ShadPS4/raw/refs/heads/main/builds/cusa04301-depth-copy-buffer-20261029/Dreams-graphical-launcher-depth-copy-buffer-20261029.zip)**

Extract the ZIP, open **shadPS4QtLauncher.exe**, add Dreams and click **Play**.

Depth/colour conversions on the fallback copy path now check the temporary buffer's capacity. A valid 144 MiB image could previously be copied through a 128 MiB buffer. Larger copies now receive a suitably sized buffer that stays alive until the GPU finishes. Small copies and the direct GPU copy path keep their existing behavior.

All [previous fixes](DEPTH_GROWTH_20261029.md), including depth-texture initialization and the Tren disappearance correction, are retained.

## Verification

- 428 GPU cases passed with zero mismatches or validation errors, checking 3,384,475,648 bytes. Coverage includes mip chains, arrays, both conversion directions, and full-size copies at and above the 128 MiB boundary.
- 192 production-code checks found 24 out-of-bounds copies before the fix and none afterward. The unsafe copies were not submitted to the GPU.
- All 384 preceding depth-growth GPU cases remain unchanged. The owner confirmed textures, shadows and movement still look correct in the private build.
- The clean release retains those production objects and removes five diagnostic objects. Both launch routes and all 45 package files are verified. The clean executable was not separately played.

Testing used AMD Radeon 8060S. The GPU replay uses real image, buffer and scheduling code with bounded cache and guest-upload fixtures. Stencil and multisample behavior are unchanged. Other physical GPUs remain untested; no performance gain or specific live artifact is attributed to this fix.

## Download and source

The graphical launcher, matching settings and optional tutorial-skip tool are included. No games, firmware, saves or creations are included.

Core SHA-256: `b35a1628895040f8b7ac8773e730eefd454e4ecc60cdd3a09a2d63aebcdc02a8`.

- [Checksums](../../builds/cusa04301-depth-copy-buffer-20261029/SHA256SUMS.txt), [verification](../../builds/cusa04301-depth-copy-buffer-20261029/verification.json), [runtime manifest](../../builds/cusa04301-depth-copy-buffer-20261029/runtime-manifest.json).
- [Source manifest](../../builds/cusa04301-depth-copy-buffer-20261029/source-manifest.json) and [new patch](../../patches/dreams-depth-copy-buffer-20261029.patch). Apply every patch in the listed order from the public base. The changed source file was reconstructed and verified; a fresh full-source rebuild is not claimed.
- [Launcher source](../../builds/cusa04301-depth-copy-buffer-20261029/DreamsCore.cs), [tutorial helper](../../builds/cusa04301-depth-copy-buffer-20261029/SkipDreamsTutorial.cs), unchanged [Qt launcher source](../../builds/cusa04301-image-cache-20261021/qt-launcher-source-99e9829.tar.gz).

The [previous package](../../builds/cusa04301-depth-growth-20261029/) remains available.
