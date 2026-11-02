# Texture synchronization update — November 1, 2026

**[Download the graphical package](https://github.com/JesusDaBest/Dreams-ShadPS4/raw/refs/heads/main/builds/cusa04301-texture-synchronization-20261101/Dreams-graphical-launcher-texture-synchronization-20261101.zip)**

Extract the ZIP, open **shadPS4QtLauncher.exe**, add Dreams and click **Play**.

Two missing GPU dependencies are corrected. Texture refresh now makes earlier buffer writes visible to the compute shader that reads tiled images, or to the transfer operation for linear images. Merging cached buffers now also synchronizes earlier writes before copying their contents into a replacement allocation. Both paths handle access tracking that has advanced before its barrier is submitted.

All [earlier fixes](READBACK_CORRECTNESS_20261101.md) remain included. **F10** toggles the FPS display, including in fullscreen.

## Verification

The texture suite passed 360 GPU cases, the buffer-merge suite passed 48, and 259 earlier regression cases passed. The old paths produced 240 and 64 synchronization-validation hazards respectively; the corrected paths produced none. Both versions returned the expected bytes on the tested GPU, so these tests establish missing dependencies rather than a visible corruption reproduction.

The user checked the combined private build across familiar scenes and reported correct textures, shadows and movement with no new pauses. The exact hazard occurrence in that live scene was not captured. No overall FPS gain is claimed.

The clean release retains the tested texture-cache object. The buffer source was rebuilt with its diagnostic scopes removed. Its 48 merge and 259 regression cases passed again with zero validation errors. The clean executable was not separately played. Both graphical launcher routes were verified without restarting the running game or writing its profile.

## Source

The [source manifest](../../builds/cusa04301-texture-synchronization-20261101/source-manifest.json) gives the complete patch order. The [new patch](../../patches/dreams-texture-synchronization-20261101.patch) reconstructs both changed files exactly from the published source stack. A fresh full-source rebuild has not been verified.
