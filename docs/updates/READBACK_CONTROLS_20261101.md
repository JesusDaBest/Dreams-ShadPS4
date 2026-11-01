# Readback and display controls — November 1, 2026

**[Download the graphical package](https://github.com/JesusDaBest/Dreams-ShadPS4/raw/refs/heads/main/builds/cusa04301-readback-controls-20261101/Dreams-graphical-launcher-readback-controls-20261101.zip)**

Extract the ZIP, open **shadPS4QtLauncher.exe**, add Dreams and click **Play**.

This update fixes image readbacks exceeding the 32 MiB staging buffer. Previously, the readback could be skipped while cache eviction still discarded the image. Oversized transfers now use a correctly sized temporary buffer, retained until the GPU copy finishes. Ordinary transfers keep the existing ring size.

Press **F10** to hide or show both FPS counters in windowed or fullscreen mode. The choice is saved between launches. Right-clicking the counter also offers **Hide FPS**; the Display menu offers **Show FPS**. The game window title is now **Dreams | shadPS4 v0.17.1 WIP**, without broken Git metadata. All [earlier fixes](MICROPHONE_RECORDING_20261031.md) are retained.

## Verification

- 24 GPU cases passed with zero validation errors, covering small/exact/oversized transfers, compressed images, mip levels, arrays, volumes and deferred lifetimes. The parent failed 14 oversized cases.
- The owner confirmed textures, shadows and movement looked correct in the private readback build. Its readback object is unchanged in this release.
- 13 native ImGui checks passed for hiding/showing, saved preferences, old settings and viewport-size changes. Both graphical and direct launcher routes passed without restarting the current game.
- The final clean executable was not separately played. No overall speedup or full fresh-source rebuild is claimed. Other physical GPUs remain untested.

## Download and source

Core SHA-256: `d0f5cb2190522f6faee096d6b51c261ef7e6a15211919f4ae2dbf3f2056c5d7f`.

The download includes the graphical launcher and optional tutorial-skip tool. It contains no game files, saves or creations.

- [Checksums](../../builds/cusa04301-readback-controls-20261101/SHA256SUMS.txt), [verification](../../builds/cusa04301-readback-controls-20261101/verification.json), [runtime manifest](../../builds/cusa04301-readback-controls-20261101/runtime-manifest.json).
- [Source manifest](../../builds/cusa04301-readback-controls-20261101/source-manifest.json), [new patch](../../patches/dreams-readback-controls-20261101.patch). Apply every listed patch in order; all three changed source files reconstruct exactly.
- [Launcher source](../../builds/cusa04301-readback-controls-20261101/DreamsCore.cs), [tutorial helper](../../builds/cusa04301-readback-controls-20261101/SkipDreamsTutorial.cs), unchanged [Qt launcher source](../../builds/cusa04301-image-cache-20261021/qt-launcher-source-99e9829.tar.gz).

The [previous package](../../builds/cusa04301-microphone-encoder-20261031/) remains available.
