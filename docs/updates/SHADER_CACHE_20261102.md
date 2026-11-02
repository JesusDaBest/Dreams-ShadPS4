# Shader-cache update — November 2, 2026

**[Download the graphical package](https://github.com/JesusDaBest/Dreams-ShadPS4/raw/refs/heads/main/builds/cusa04301-shader-cache-20261102/Dreams-graphical-launcher-shader-cache-20261102.zip)**

Extract the ZIP, open **shadPS4QtLauncher.exe**, add Dreams and click **Play**.

Shaders are saved as you encounter them, then preloaded on later launches to reduce first-use pauses. Cache reload now restores the recorded shader variant and preserves the helper stage used for depth-only clipping. Unseen shaders can still compile during play; this does not preload every shader in Dreams on the first launch.

This release also includes the tested buffer-copy and image-to-buffer synchronization corrections. All [earlier fixes](TEXTURE_SYNCHRONIZATION_20261101.md) are retained. **F10** toggles the FPS display, including in fullscreen.

## Verification

The user reported **“No pause; visuals correct”** after entering Sculpt mode. All 331 saved pipelines preloaded, with none skipped. The saved-cache replay restored 312 of 312 pipelines and checked 354 shader variants, 28 pair orders, 28 duplicate reuses and 140 conflict guards. Cache rendering passed 10 GPU cases comparing 6,144 depth pixels. The copy suites passed 60 and 432 GPU cases; 259 buffer regression cases also passed, with no validation errors.

The clean package retains the exact tested pipeline objects and rebuilds the buffer source with only its diagnostic scopes removed. Its cache and buffer regression checks passed again. Both graphical launcher routes were verified without restarting the user's game or writing their profile. The clean executable was not separately played. No overall FPS gain is claimed.

The package contains no games, saves or prebuilt game shader caches. Its writable shader cache is created inside the runtime folder. The existing tutorial helper remains included.

## Source

The [source manifest](../../builds/cusa04301-shader-cache-20261102/source-manifest.json) lists the complete patch order. The [new patch](../../patches/dreams-shader-cache-20261102.patch) reconstructs all four changed files exactly from the published stack. A fresh full-source rebuild has not been verified.
