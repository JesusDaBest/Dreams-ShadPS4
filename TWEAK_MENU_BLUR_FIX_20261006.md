# Tweak-menu blur correction — October 6, 2026

Dreams' glass mip builder (`0x345893a7`, PrimGlassMipBlurDownCS) supplies texel coordinates and an explicit source mip. The emulator normalized these coordinates using mip-zero dimensions. This targeted correction uses the selected source mip's dimensions, preserving the destination mip write. It gives the enabled shader a separate binary cache key.

## Validation

The source compiled and linked successfully with a single build worker. Inspection of the active translated shader confirmed all four sampling paths query dimensions at their source LOD, and the destination mip write is preserved. The user confirmed the tested tweak menu looks correct at close, normal and far distances. Other gadget menus, GPUs and game versions have not been validated; this is not a complete Dreams rendering fix.

## Download

- [Exact tested executable](builds/cusa04301-tweak-menu-blur-fix-20261006/shadps4.exe)
- [Validation](builds/cusa04301-tweak-menu-blur-fix-20261006/validation.json) and [SHA-256](builds/cusa04301-tweak-menu-blur-fix-20261006/SHA256SUMS.txt)
- [Focused source patch](patches/dreams-tweak-menu-blur-fix-20261006.patch)
- [Cumulative main-source diagnostic snapshot](patches/dreams-diagnostic-source-20261006-tweak-menu-blur.patch)

Download the EXE and use it with your existing launcher/runtime dependencies and Precise GPU readbacks. The tweak-menu correction is enabled automatically; no activation script or environment setting is required. No game files or raw proprietary shader dumps are included.

This default-enabled executable compiled and linked successfully. The visual all-distance test used the preceding executable with the identical correction enabled through its test setting. This is an experimental emulator executable, not a complete portable runtime. The cumulative snapshot includes earlier experimental changes; the focused patch isolates this correction. Retain the separately documented external-submodule patches when building from source. EXE SHA-256: `d3eb8e1893dc7078a6ef5478091471554de6de3b895a37dce9e6551811eeef73`.
