# Texture readback lifetime — November 1, 2026

**[Download the graphical package](https://github.com/JesusDaBest/Dreams-ShadPS4/raw/refs/heads/main/builds/cusa04301-readback-lifetime-20261101/Dreams-graphical-launcher-readback-lifetime-20261101.zip)**

Extract the ZIP, open **shadPS4QtLauncher.exe**, add Dreams and click **Play**.

This update prevents texture data from being overwritten while a cache-eviction writeback is still pending. The shared download ring waited for GPU completion but could reuse the same space before the CPU had copied the result. Asynchronous readbacks now retain their own temporary buffer until writeback finishes. Ordinary synchronous readbacks keep the existing 32 MiB ring.

All [earlier fixes and display controls](READBACK_CONTROLS_20261101.md) are retained. **F10** still hides or shows the FPS counters, including in fullscreen, and remembers the choice.

## Verification

- 24 GPU cases passed with zero validation errors. The previous code failed five cases when a later transfer reused the ring during delayed CPU writeback.
- Coverage includes mip chains, arrays, volumes, compressed formats, image padding, oversized transfers and synchronous readback. Temporary allocations were released after completion.
- The owner confirmed textures, shadows and movement looked correct in the private build. The tested texture-cache object is unchanged in this clean release.
- Both graphical and direct launcher routes were verified without restarting the running game. The clean executable was not separately played. No overall speedup or full fresh-source rebuild is claimed; other physical GPUs remain untested. Asynchronous eviction now has a temporary allocation cost.

## Download and source

Core SHA-256: `84c4000b4970fba2632de1cf2132a6222ad6fa9e4e32c1d23c822d5157ee2a34`.

The package includes the graphical launcher and optional tutorial-skip tool. It contains no game files, saves or creations.

- [Checksums](../../builds/cusa04301-readback-lifetime-20261101/SHA256SUMS.txt), [verification](../../builds/cusa04301-readback-lifetime-20261101/verification.json), [runtime manifest](../../builds/cusa04301-readback-lifetime-20261101/runtime-manifest.json).
- [Source manifest](../../builds/cusa04301-readback-lifetime-20261101/source-manifest.json), [new patch](../../patches/dreams-readback-lifetime-20261101.patch). Apply every listed patch in order; the changed cache source reconstructs exactly.
- [Launcher source](../../builds/cusa04301-readback-lifetime-20261101/DreamsCore.cs), [tutorial helper](../../builds/cusa04301-readback-lifetime-20261101/SkipDreamsTutorial.cs), unchanged [Qt launcher source](../../builds/cusa04301-image-cache-20261021/qt-launcher-source-99e9829.tar.gz).

The [previous package](../../builds/cusa04301-readback-controls-20261101/) remains available.
