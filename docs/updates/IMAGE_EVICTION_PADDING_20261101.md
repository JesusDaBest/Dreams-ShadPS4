# Overlapping texture correction — November 1, 2026

**[Download the graphical package](https://github.com/JesusDaBest/Dreams-ShadPS4/raw/refs/heads/main/builds/cusa04301-image-eviction-padding-20261101/Dreams-graphical-launcher-image-eviction-padding-20261101.zip)**

Extract the ZIP, open **shadPS4QtLauncher.exe**, add Dreams and click **Play**.

When freeing overlapping textures, padding saved before an earlier image finished writing back could overwrite that image's pixels. The fix refreshes the affected padding after earlier writebacks complete. Independent images still download together; clean-only eviction adds no GPU wait.

All [earlier fixes](IMAGE_EVICTION_WRITEBACK_20261101.md) remain included. **F10** shows or hides the FPS counters, including in fullscreen, and remembers your choice.

## Verification

The correction passed 184 native GPU checks with no validation errors, including overlapping colour, compressed and depth images; mip levels and array layers; failed transfers; and the previous CPU-write/remapping protections. The baseline lost data in 25 overlapping-image cases; the correction preserved it in all of them. Another 20,000 cache-selection comparisons preserved writeback order, eviction limits and memory accounting. The tests use bounded adapters on one physical GPU.

The private build passed the user's familiar-scene check with correct textures, shadows and movement and no new pauses reported. That check did not prove this exact eviction case executed. The clean package uses the exact tested texture-cache object with five established diagnostic objects removed or replaced. Both graphical and direct launcher routes were checked without starting another game session. The clean executable was not separately played.

Overlapping padded evictions now wait in order. Independent-image transfer timing was checked across 108 batches; results varied in both directions. This is not a Dreams FPS comparison, and no overall speedup is claimed.

## Source

The [source manifest](../../builds/cusa04301-image-eviction-padding-20261101/source-manifest.json) gives the complete patch order. The [new patch](../../patches/dreams-image-eviction-padding-20261101.patch) changes one source file, verified to reconstruct exactly from the public base and patch stack. A full fresh-source build has not been verified.
