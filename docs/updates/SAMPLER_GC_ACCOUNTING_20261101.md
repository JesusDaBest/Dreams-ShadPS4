# Sampler cleanup correction - November 1, 2026

**[Download the graphical package](https://github.com/JesusDaBest/Dreams-ShadPS4/raw/refs/heads/main/builds/cusa04301-sampler-gc-accounting-20261101/Dreams-graphical-launcher-sampler-gc-accounting-20261101.zip)**

Extract the ZIP, open **shadPS4QtLauncher.exe**, add Dreams and click **Play**.

Sampler cleanup kept using its original entry count after removing old entries. Its second pass could therefore discard more cached samplers even after the count had fallen below the critical threshold. The fix updates that count after each removal. Existing cleanup limits and GPU-safe deferred destruction are retained; it adds no GPU wait.

All [earlier fixes](IMAGE_EVICTION_PADDING_20261101.md) remain included. **F10** shows or hides the FPS counters, including in fullscreen, and remembers your choice.

## Verification

The correction passed 1,224 cache-selection comparisons. The old code disagreed with the independent expected result in 94 cases; the corrected code matched every case. In one example, cleanup should keep 10 of 30 entries after removing 20 old entries. The old code unnecessarily removed those remaining 10; the correction retained them.

The candidate passed 72 native GPU checks, alongside 72 baseline controls, with correct sampling and no validation or lifetime errors. These checks use bounded cache thresholds on one physical GPU.

The private build passed the user's familiar-scene check: textures, shadows and movement looked correct, with no new pauses. This did not prove the specific cleanup threshold occurred in that scene. No overall FPS improvement is claimed.

The clean package uses the same tested texture-cache object with five established diagnostic objects removed or replaced. Both graphical and direct launcher routes were verified without starting another game session. The clean executable was not separately played.

## Source

The [source manifest](../../builds/cusa04301-sampler-gc-accounting-20261101/source-manifest.json) gives the complete patch order. The [new patch](../../patches/dreams-sampler-gc-accounting-20261101.patch) adds one accounting update and a comment in one source file, verified to reconstruct exactly from the public base and patch stack. A full fresh-source build has not been verified.
