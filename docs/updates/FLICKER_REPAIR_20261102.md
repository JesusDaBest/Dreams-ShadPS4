# Flicker repair — November 2, 2026

**[Download the graphical package](https://github.com/JesusDaBest/Dreams-ShadPS4/raw/refs/heads/main/builds/cusa04301-flicker-repair-20261102/Dreams-graphical-launcher-flicker-repair-20261102.zip)**

Extract the ZIP, open **shadPS4QtLauncher.exe**, add Dreams and click **Play**.

This release removes two extra readback calls added before image hashing. The user reported that the returning flicker disappeared with those calls removed. Normal GPU readback remains. The safe-boundary buffer-lifetime repair is also retained: completed fault ranges are resolved at the next submission boundary, rather than replacing buffers during draw preparation.

All other [latest fixes](SHADER_CACHE_20261102.md), including shader preloading, remain. **F10** toggles the FPS display in windowed or fullscreen mode.

## Verification

The tested private build preloaded all 336 saved pipelines. Its other 392 link inputs were unchanged by the readback comparison. The clean release keeps the same fault-management and shader-pipeline objects, removes the custom debugging tools and two one-time comparison messages, and passes 259 buffer regression cases plus 10 cache GPU cases with no validation errors. The retained fault-boundary fixture previously passed 16 cases, eliminating three stale bindings; that repair alone did not resolve the reported flicker.

Both graphical launcher routes were verified without restarting the user's game or writing their profile. The clean executable was not separately played. The specific defect within the removed batching optimization has not been isolated, and no FPS improvement or absence of performance cost is claimed.

The download contains no games, saves or prebuilt game shader caches. The existing tutorial helper remains included.

## Source

The [source manifest](../../builds/cusa04301-flicker-repair-20261102/source-manifest.json) lists the complete patch order. The [new patch](../../patches/dreams-flicker-repair-20261102.patch) reconstructs both changed files exactly from the published stack. A fresh full-source rebuild has not been verified.
