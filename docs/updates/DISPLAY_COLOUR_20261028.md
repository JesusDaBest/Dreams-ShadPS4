# Display colour correction — October 28, 2026

**[Download the graphical launcher package](https://github.com/JesusDaBest/Dreams-ShadPS4/raw/refs/heads/main/builds/cusa04301-display-colour-20261028/Dreams-graphical-launcher-display-colour-20261028.zip)**

Extract the ZIP, open **shadPS4QtLauncher.exe**, add Dreams and click **Play**.

This retains the [previous fixes](DYNAMIC_MIP_20261028.md) and corrects the extra colour conversion that made Dreams look too bright. Dreams supplies an sRGB-encoded 10-bit display image, but Vulkan samples that format through a UNORM view. The final pass now recognizes that the colours are already encoded, instead of encoding them again. Non-neutral gamma settings still work; other display formats retain their existing handling.

Core SHA-256: `57efb60895ba676802e8fec7713f54a2befd44fbaab28f26c1ffd07503611de0`.

## Verification

- The old path produced 38,380 wrong sample values, all in the 10 affected 10-bit sRGB cases. All 40 unaffected controls already passed.
- The correction passes all 50 GPU comparisons and 256,000 sample values, with zero mismatches or validation errors. Coverage includes five display formats, five gamma values, native-resolution channel ramps, and constant colours through production FSR with RCAS.
- The actual 10-bit sRGB display path was verified in running Dreams. The owner confirmed correct brightness and no new colour problems with this exact executable.
- Four production source files and two linked objects change. No render pass or image was added. Guest shader compilation, shadow fixes and texture caches are unchanged.

GPU testing used AMD Radeon 8060S and a floating-point target to isolate colour math; final 8-bit quantization and other physical GPUs were not tested. FSR without RCAS was source-audited; GPU FSR tests used RCAS. No same-frame PS4 comparison or performance gain is claimed.

## Download and source

The graphical launcher, matching settings and optional tutorial-skip tool are included. The package contains no games, firmware, saves or creations.

- [Checksums](../../builds/cusa04301-display-colour-20261028/SHA256SUMS.txt), [verification](../../builds/cusa04301-display-colour-20261028/verification.json), [runtime manifest](../../builds/cusa04301-display-colour-20261028/runtime-manifest.json).
- [Source manifest](../../builds/cusa04301-display-colour-20261028/source-manifest.json) and [new patch](../../patches/dreams-display-colour-20261028.patch). Apply the listed patches in order from the public base. The four changed source files were reconstructed from the public base and preceding patches, then verified with this patch; a fresh full-source rebuild is not claimed.
- [Launcher adapter source](../../builds/cusa04301-display-colour-20261028/DreamsCore.cs), [tutorial helper source](../../builds/cusa04301-display-colour-20261028/SkipDreamsTutorial.cs), unchanged [Qt launcher source](../../builds/cusa04301-image-cache-20261021/qt-launcher-source-99e9829.tar.gz).

The 45 package files are allowlisted and hash-verified. Both launch routes were checked without starting another game. The [previous package](../../builds/cusa04301-dynamic-mip-20261028/) remains available.
