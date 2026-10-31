# Texture readback correction — October 30, 2026

**[Download the graphical launcher package](https://github.com/JesusDaBest/Dreams-ShadPS4/raw/refs/heads/main/builds/cusa04301-mip-readback-20261030/Dreams-graphical-launcher-mip-readback-20261030.zip)**

Extract the ZIP, open **shadPS4QtLauncher.exe**, add Dreams and click **Play**.

Successful GPU-to-CPU texture readback now updates the mip baselines from the completed download. This prevents a later CPU edit from causing unchanged GPU-rendered mip data to be replaced. Failed readbacks leave those baselines alone. Deferred writebacks now own their callbacks until execution, fixing a dangling callback reference during image retirement.

All [previous rendering, shadow, texture-cache and Tren fixes](MIP_REFRESH_20261030.md) are retained.

## Verification

- All 264 GPU cases passed with no pixel mismatches, stale baselines or validation errors, including a repeat using the clean release's production link. The original synchronous reproduction failed 96 of 216 cases.
- Coverage includes RGBA8, BC1 and BC3, two/three mips, one/two layers, successful and failed backing copies, CPU writes immediately after readback, repeated downloads and 48 deferred image-retirement cases. Three callback-lifetime checks distinguish the old reference from the owned callback. The unsafe old deferred callback was excluded from repeated GPU execution after its lifetime failure was reproduced.
- The owner confirmed textures, shadows and movement still look correct. The release retains the exact tested texture-cache object and the preceding clean rendering objects. Both graphical and direct launcher routes were verified without starting another game.

The clean executable was not separately played. Tests use complete production readback/refresh methods and native scheduling with bounded guest-memory and staging adapters. Detiling, multisample behavior and other physical GPUs were not newly certified. No specific visible artifact or gameplay speedup is attributed to this change. A small FPS reduction was reported after the preceding mip-refresh release; its cause and cost remain unconfirmed.

## Download and source

The graphical launcher, matching settings and optional tutorial-skip tool are included. No games, firmware, saves or creations are included.

Core SHA-256: `e33e27de34e13a1da59833d99a9a4820b28378c04bd472b31e4cea58ec332b71`.

- [Checksums](../../builds/cusa04301-mip-readback-20261030/SHA256SUMS.txt), [verification](../../builds/cusa04301-mip-readback-20261030/verification.json), [runtime manifest](../../builds/cusa04301-mip-readback-20261030/runtime-manifest.json).
- [Source manifest](../../builds/cusa04301-mip-readback-20261030/source-manifest.json) and [new patch](../../patches/dreams-mip-readback-20261030.patch). Apply every patch in order from the listed public base. The changed file was reconstructed and verified; a fresh full-source rebuild is not claimed.
- [Launcher source](../../builds/cusa04301-mip-readback-20261030/DreamsCore.cs), [tutorial helper](../../builds/cusa04301-mip-readback-20261030/SkipDreamsTutorial.cs), unchanged [Qt launcher source](../../builds/cusa04301-image-cache-20261021/qt-launcher-source-99e9829.tar.gz).

The [previous package](../../builds/cusa04301-mip-refresh-20261030/) remains available.
