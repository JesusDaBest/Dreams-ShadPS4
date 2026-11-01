# Memory-copy correction — November 1, 2026

**[Download the graphical package](https://github.com/JesusDaBest/Dreams-ShadPS4/raw/refs/heads/main/builds/cusa04301-backing-write-segments-20261101/Dreams-graphical-launcher-backing-write-segments-20261101.zip)**

Extract the ZIP, open **shadPS4QtLauncher.exe**, add Dreams and click **Play**.

GPU readbacks that crossed multiple physical memory regions could repeat the first block of data in later regions. This update advances the source pointer correctly, bounds the region iterator, and stops reporting an incomplete copy as successful.

All [earlier fixes and display controls](READBACK_LIFETIME_20261101.md) are retained. **F10** hides or shows the FPS counters, including in fullscreen, and remembers the choice.

## Verification

- 2,526 memory-copy checks passed. The previous code failed 720 split-copy checks and incorrectly reported three incomplete copies as successful.
- Cases cover direct, flexible and pooled memory, multiple virtual regions, reordered physical blocks, boundary offsets, incomplete backing and untouched-byte guards. These are CPU checks, not GPU-case counts.
- The exact baseline memory object was reproduced before applying the correction. The tested memory object is unchanged in the clean release.
- The owner confirmed textures, shadows and movement looked correct in the private build. Both launcher routes were verified without starting the game. The clean executable was not separately played.
- No overall FPS gain is claimed. Deferred write ordering and memory reuse are separate audit topics.

## Source

The [source manifest](../../builds/cusa04301-backing-write-segments-20261101/source-manifest.json) lists the complete patch order. The [new patch](../../patches/dreams-backing-write-segments-20261101.patch) changes one function in `src/core/memory.cpp`; that source reconstructs exactly from the published base and patch stack. Full fresh-build reproducibility has not been established.
