# Texture writeback correction — November 1, 2026

**[Download the graphical package](https://github.com/JesusDaBest/Dreams-ShadPS4/raw/refs/heads/main/builds/cusa04301-image-eviction-writeback-20261101/Dreams-graphical-launcher-image-eviction-writeback-20261101.zip)**

Extract the ZIP, open **shadPS4QtLauncher.exe**, add Dreams and click **Play**.

When the texture cache freed an image, a delayed GPU readback could overwrite a newer CPU edit or a replacement memory mapping. The corrected path keeps the old image protected until its readback reaches memory, then frees it. Dirty images are completed together in one batch; staging allocations are disposed of on the existing background cleanup thread. Clean-only eviction does not add a GPU wait.

All [earlier fixes](GPU_RESOURCE_LIFETIME_20261101.md) remain included. **F10** shows or hides the FPS counters, including in fullscreen, and remembers your choice.

## Verification

The correction passed 56 native GPU cases with no validation errors. The previous code lost newer data in 20 cases; the corrected code preserved it in all of them, including real Windows page faults and same-address remapping. Another 20,000 mixed cache-pressure comparisons preserved eviction selection, budgets and memory accounting. Tests use bounded adapters around guest memory and cache state on one physical GPU.

The private build passed the user's familiar-scene check with correct textures, shadows and movement and no new pauses reported. That check did not prove the pressured-eviction path executed. The clean package uses the exact tested texture-cache object with five established diagnostic objects removed or replaced. Both graphical and direct launcher routes were checked without starting another game session. The clean executable was not separately played.

Dirty eviction now waits for its batch before releasing protection. The transfer benchmark covered 108 batches, but is not a game FPS comparison; no overall speedup or absence of all stalls is claimed.

## Source

The [source manifest](../../builds/cusa04301-image-eviction-writeback-20261101/source-manifest.json) gives the complete patch order. The [new patch](../../patches/dreams-image-eviction-writeback-20261101.patch) changes three source files, all verified to reconstruct exactly from the public base and patch stack. A full fresh-source build has not been verified.
