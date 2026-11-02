# Readback correctness update — November 1, 2026

**[Download the graphical package](https://github.com/JesusDaBest/Dreams-ShadPS4/raw/refs/heads/main/builds/cusa04301-readback-correctness-20261101/Dreams-graphical-launcher-readback-correctness-20261101.zip)**

Extract the ZIP, open **shadPS4QtLauncher.exe**, add Dreams and click **Play**.

This update combines the approved GPU readback repairs: access existing cached ranges without merging unrelated buffers, restore pending shader writes after preparation-time reads, retain vertex input state through preparation, and revalidate buffer identities after operations that can submit GPU work. Busy upload, tiling-parameter and download reservations use owned temporary storage so callbacks cannot retire a source while its copy is being prepared. Completion callbacks can reenter safely, including when they submit more GPU work.

All [earlier fixes](SAMPLER_GC_ACCOUNTING_20261101.md) remain included. **F10** toggles the FPS display, including in fullscreen.

## Verification

The combined private candidate passed 259 regression cases with zero Vulkan validation errors. The same 259 cases passed with the clean release objects. These cover cached ranges, GPU readback, busy staging storage, source replacement and nested completion callbacks on one GPU. They do not cover every rendering path.

The user explicitly requested promotion after the candidate was opened for testing; a separate visual result was not supplied. The clean executable was not separately played. No overall FPS gain or complete resolution of the earlier visual regression is claimed.

Five diagnostic objects were replaced or rebuilt. The texture-cache and tiling objects are identical to the private candidate; rebuilt renderer, buffer and scheduler sources retain its production changes. Both graphical launcher routes were verified without restarting the running game or writing to its profile.

## Source

The [source manifest](../../builds/cusa04301-readback-correctness-20261101/source-manifest.json) gives the complete patch order. The [new patch](../../patches/dreams-readback-correctness-20261101.patch) reconstructs six changed files exactly from the published source stack. A fresh full-source rebuild has not been verified.
