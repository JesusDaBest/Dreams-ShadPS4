# Texture copy and resolve update — October 27, 2026

**[Download the graphical launcher package](https://github.com/JesusDaBest/Dreams-ShadPS4/raw/refs/heads/main/builds/cusa04301-copy-resolve-20261027/Dreams-graphical-launcher-copy-resolve-20261027.zip)**

Extract the ZIP, open **shadPS4QtLauncher.exe**, add Dreams and click **Play**.

This keeps the [previous fixes](COPY_WIDTH_20261027.md) and adds three corrections in two functions:

- Fallback texture copies respect a narrower destination's visible width at each mip.
- Valid array expansion across multiple mips no longer triggers an assertion.
- Texture copies and multisample resolves stay within both selected source and destination mip dimensions when their cached backings differ.

Resolution and detail settings are unchanged. No overall speedup is claimed.

Core SHA-256: `32009851f315253307eb7fda4dd483418fe7335958bbd7cb969ed81cce3ce6cb`.

## Verification

- Fallback: 640 reachable CPU cases remove 240 assertions and 320 invalid regions; 480 previously valid regions are unchanged. 768 native GPU cases and 320 legal parent controls pass.
- Resolve: 1,152 CPU cases reduce 192 invalid extents to zero; 960 previously valid cases are unchanged. 864 native GPU cases and 384 legal parent controls pass.
- No GPU validation errors in those tests. They check copied data and untouched destination pixels, layers and mips. Fallback tests additionally check source data and buffer guards.
- One source file and linked object change. The two private compilation steps were checked against their parents; the tested fallback function is unchanged in the final candidate. All other compiled objects retain the preceding fixes.
- The owner checked the same executable in a familiar scene and reported correct shadows, textures and movement, with no new flickering or disappearing objects.

Tests used AMD Radeon 8060S. Its normal game path bypasses the fallback because it supports VK_KHR_maintenance8; the GPU fixture exercised that fallback directly. Other physical GPUs remain unverified. No particular live glitch or FPS gain is attributed to these defects.

Fallback coverage includes 16/32-bit depth-to-colour and colour-to-depth array copies, odd widths, unequal layer/mip counts and linear/tiled guest layouts. Resolve coverage uses RGBA8 arrays, 1/4 samples, differing extents and selected mip/layer ranges. Multisample tests use equal values per sample and do not test averaging arithmetic. Invalid parent requests were checked on the CPU rather than submitted to the driver. Full guest command streams, other formats, partial resolves and the broader mip audit remain outside these checks.

## Download and source

The graphical launcher, matching settings and optional tutorial-skip tool are included. The package contains no games, firmware, saves or creations.

- [Checksums](../../builds/cusa04301-copy-resolve-20261027/SHA256SUMS.txt), [verification](../../builds/cusa04301-copy-resolve-20261027/verification.json), [runtime manifest](../../builds/cusa04301-copy-resolve-20261027/runtime-manifest.json).
- [Source manifest](../../builds/cusa04301-copy-resolve-20261027/source-manifest.json) and [new patch](../../patches/dreams-copy-resolve-20261027.patch). Apply the listed patches in order from the public base. Patch application and source hashes are verified; a fresh full-source rebuild is not claimed.
- [Launcher adapter source](../../builds/cusa04301-copy-resolve-20261027/DreamsCore.cs), [tutorial helper source](../../builds/cusa04301-copy-resolve-20261027/SkipDreamsTutorial.cs), unchanged [Qt launcher source](../../builds/cusa04301-image-cache-20261021/qt-launcher-source-99e9829.tar.gz).

The 45 package files are allowlisted and hash-verified. Both launch routes were checked without starting another game. The [previous package](../../builds/cusa04301-copy-width-20261027/) remains available.
