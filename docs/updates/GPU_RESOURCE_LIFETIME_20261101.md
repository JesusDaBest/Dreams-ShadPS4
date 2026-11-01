# GPU resource corrections — November 1, 2026

**[Download the graphical package](https://github.com/JesusDaBest/Dreams-ShadPS4/raw/refs/heads/main/builds/cusa04301-gpu-resource-lifetime-20261101/Dreams-graphical-launcher-gpu-resource-lifetime-20261101.zip)**

Extract the ZIP, open **shadPS4QtLauncher.exe**, add Dreams and click **Play**.

This release includes four corrections:

- Buffer readbacks now allocate sufficient storage for large transfers and keep deferred transfer data alive until the CPU writeback finishes.
- Cache cleanup respects a callback's stop result instead of continuing to scan unrelated entries.
- Samplers removed from the cache stay alive until the GPU has finished using them.
- Transfers in the same GPU submission extend the previous tracking record instead of allocating a separate record for every transfer.

All [earlier fixes](BACKING_WRITE_SEGMENTS_20261101.md) remain included. **F10** shows or hides the FPS counters, including in fullscreen, and remembers your choice.

## Verification

The changes passed 102 native GPU cases with no validation errors, 6,921 LRU assertions and 160 comparisons of production cache-cleanup behavior. The sampler tests cover recorded and pending submissions; buffer tests cover large, offset and deferred transfers. A 50,000-transfer batch needed one tracking record instead of 50,000.

The clean package keeps two production object files from the private candidate and rebuilds the buffer-cache file with only its diagnostic scopes removed. The previous clean buffer object was reproduced exactly before rebuilding it. Four other diagnostic objects are replaced by their established clean counterparts. Both graphical and direct launcher routes were verified without starting another game session. Promotion was authorized after the private build was opened for testing; the clean executable was not separately played. No overall FPS gain is claimed. Deferred image writeback ordering remains a separate unresolved audit.

## Source

The [source manifest](../../builds/cusa04301-gpu-resource-lifetime-20261101/source-manifest.json) gives the complete patch order. The [new patch](../../patches/dreams-gpu-resource-lifetime-20261101.patch) changes four source files, all verified to reconstruct exactly from the public base and patch stack. A full fresh-source build has not been verified.
