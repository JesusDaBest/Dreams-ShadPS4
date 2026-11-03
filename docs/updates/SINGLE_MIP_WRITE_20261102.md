# Texture-write update — November 2, 2026

**[Download the graphical package](https://github.com/JesusDaBest/Dreams-ShadPS4/raw/refs/heads/main/builds/cusa04301-single-mip-write-20261102/Dreams-graphical-launcher-single-mip-write-20261102.zip)**

Extract the ZIP, open **shadPS4QtLauncher.exe**, add Dreams and click **Play**.

A definite CPU write now refreshes a single-mip texture even when it restores the same bytes as an older CPU snapshot. Single-mip ownership and immediate-download paths no longer compute that per-mip snapshot. Multi-mip preservation and shared-page uncertainty checks remain.

All [previous fixes](FLICKER_REPAIR_20261102.md), shader preloading, microphone recording and display controls are retained. **F10** toggles the FPS display.

## Verification and remaining issue

The test suite passed 528 GPU cases and 16 protected-memory checks with no Vulkan validation errors. The earlier code failed 48 same-value CPU-write cases in the new fixture. These tests use production refresh methods and native Vulkan transfers with a bounded source-buffer adapter; they do not cover every interaction between CPU pages and GPU-owned image data.

In the live candidate, the user reported that the growing black patch inside tweak menus was gone, but another visual effect remained. Its cause is still unresolved. The user explicitly requested promotion with that issue outstanding. This is not an all-clear visual result, and no matched FPS improvement has been established.

The clean package uses the tested production objects and excludes custom diagnostic tools. Its two graphical launcher routes and package contents were verified without restarting the running game or writing the user's profile. The clean executable was not separately played. The download contains no games, saves or prebuilt game shader caches; the existing tutorial helper remains included.

## Source

The [source manifest](../../builds/cusa04301-single-mip-write-20261102/source-manifest.json) gives the complete patch order. The [new patch](../../patches/dreams-single-mip-write-20261102.patch) reconstructs both changed files exactly from the published stack. A fresh full-source rebuild has not been verified.
