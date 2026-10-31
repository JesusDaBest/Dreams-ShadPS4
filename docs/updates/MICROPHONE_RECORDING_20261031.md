# Microphone recording update — October 31, 2026

**[Download the graphical package](https://github.com/JesusDaBest/Dreams-ShadPS4/raw/refs/heads/main/builds/cusa04301-microphone-encoder-20261031/Dreams-graphical-launcher-microphone-encoder-20261031.zip)**

Extract the ZIP, open **shadPS4QtLauncher.exe**, add Dreams and click **Play**.

Sound gadget recordings could capture input but fail to produce playable audio because the recording encoder was unimplemented. This update adds a software ATRAC9 encoder and connects its memory, initialization, reset and encoding APIs. The owner confirmed a recording was created and audible. All [earlier fixes](PACKED_IMAGE_FORMATS_20261031.md) are retained.

Select a working microphone in Settings. **None** disables input; the encoder cannot supply sound from a silent device.

## Verification

The release uses the exact encoder and registration objects from the successful private recording test. All rendering objects remain unchanged. Checks include 20 guest API cases, 10 independently decoded mono/stereo audio cases, 10 chunk-invariance cases and 12 interleaved-context checks. Each recording context uses 20,504 bytes inside the game's existing memory allowance; encoding scratch is reused per thread. Both graphical and direct launcher routes pass without starting or restarting a game.

Supported configurations are Dreams' observed 48 kHz PCM16 mono/96 kbps and stereo/192 kbps parameters. Others return failure. This is a software implementation, not a claim of bit-identical Sony encoder output. The clean executable was not separately played; a fresh full-source rebuild is not claimed.

## Download and source

The package includes the graphical launcher, matching settings and optional tutorial-skip tool. No games, firmware, saves or creations are included.

Core SHA-256: `d85e467c51155eb6cf88d4bf249574e1a8cb5d15912039f9c4ecbefcf97d2378`.

- [Checksums](../../builds/cusa04301-microphone-encoder-20261031/SHA256SUMS.txt), [verification](../../builds/cusa04301-microphone-encoder-20261031/verification.json), [runtime manifest](../../builds/cusa04301-microphone-encoder-20261031/runtime-manifest.json).
- [Source manifest](../../builds/cusa04301-microphone-encoder-20261031/source-manifest.json) and [new patch](../../patches/dreams-microphone-encoder-20261031.patch). Apply all listed patches in order. All changed and added files were reconstructed and verified.
- The encoder derives from [PS5 ArtSwap](https://github.com/griches/PS5_ArtSwap/tree/03a5c40b9b34bba20deaae46aa0de272f21065cf), under GPL-3.0-or-later, with LibAtrac9 table notices retained. Source and attribution are in the patch; licenses are also included in the download.
- [Launcher source](../../builds/cusa04301-microphone-encoder-20261031/DreamsCore.cs), [tutorial helper](../../builds/cusa04301-microphone-encoder-20261031/SkipDreamsTutorial.cs), unchanged [Qt launcher source](../../builds/cusa04301-image-cache-20261021/qt-launcher-source-99e9829.tar.gz).

The [previous package](../../builds/cusa04301-packed-image-formats-20261031/) remains available.
