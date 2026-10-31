# Texture-border cache correction — October 30, 2026

**[Download the graphical launcher package](https://github.com/JesusDaBest/Dreams-ShadPS4/raw/refs/heads/main/builds/cusa04301-sampler-border-20261030/Dreams-graphical-launcher-sampler-border-20261030.zip)**

Extract the ZIP, open **shadPS4QtLauncher.exe**, add Dreams and click **Play**.

Changing a custom texture-border colour could reuse a sampler holding the old colour. The cache key now includes the selected RGBA value, so changing the table or updating its contents selects the correct sampler. Unchanged colours still reuse cached samplers. Predefined borders keep their existing cache keys.

All [previous rendering, shadow, texture-cache and Tren fixes](TEXTURE_COPY_STATE_20261029.md) are retained.

## Verification

- 180 native GPU samples passed with no wrong border components, interior-pixel errors, guard errors or validation errors. The preceding code failed 72 samples; the corrected code failed none.
- Checks cover nearest/linear filtering, all three mip modes, low/high table indices, table switches, in-place colour and alpha changes, equal-colour reuse, predefined borders and compare-key separation.
- The owner confirmed textures, shadows and movement still look correct in the private build.
- The clean release retains the tested production objects and removes five diagnostic objects. Both launch routes and all 45 package files are verified. The clean executable was not separately played.

The GPU tests run the production cache method and native sampler, with bounded cache-owner and LRU fixtures. The unsupported-custom-border fallback was reviewed but not exercised on this GPU. No particular visible Dreams artifact or speedup is attributed to this fix, and other physical GPUs remain untested.

## Download and source

The graphical launcher, matching settings and optional tutorial-skip tool are included. No games, firmware, saves or creations are included.

Core SHA-256: `7a158a581d146fed1351045c3da7d843df3856bbdf68596d01105d5c856bf18e`.

- [Checksums](../../builds/cusa04301-sampler-border-20261030/SHA256SUMS.txt), [verification](../../builds/cusa04301-sampler-border-20261030/verification.json), [runtime manifest](../../builds/cusa04301-sampler-border-20261030/runtime-manifest.json).
- [Source manifest](../../builds/cusa04301-sampler-border-20261030/source-manifest.json) and [new patch](../../patches/dreams-sampler-border-20261030.patch). Apply every patch in the listed order from the public base. The changed source was reconstructed and verified; a fresh full-source rebuild is not claimed.
- [Launcher source](../../builds/cusa04301-sampler-border-20261030/DreamsCore.cs), [tutorial helper](../../builds/cusa04301-sampler-border-20261030/SkipDreamsTutorial.cs), unchanged [Qt launcher source](../../builds/cusa04301-image-cache-20261021/qt-launcher-source-99e9829.tar.gz).

The [previous package](../../builds/cusa04301-texture-copy-state-20261029/) remains available.
