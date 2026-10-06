# Tweak-menu blur correction — October 6, 2026

Dreams' glass mip builder (`0x345893a7`, PrimGlassMipBlurDownCS) supplies texel coordinates and an explicit source mip. The emulator normalized these coordinates using mip-zero dimensions. This targeted correction uses the selected source mip's dimensions, preserving the destination mip write. It gives the enabled shader a separate binary cache key.

## Validation

The source compiled and linked successfully with a single build worker. Inspection of the active translated shader confirmed all four sampling paths query dimensions at their source LOD, and the destination mip write is preserved. The user confirmed the tested tweak menu looks correct at close, normal and far distances. Other gadget menus, GPUs and game versions have not been validated; this is not a complete Dreams rendering fix.

## Download and enable

- [Exact tested executable](builds/cusa04301-tweak-menu-blur-fix-20261006/shadps4.exe)
- [Launch script](builds/cusa04301-tweak-menu-blur-fix-20261006/launch-with-tweak-menu-fix.cmd)
- [Validation](builds/cusa04301-tweak-menu-blur-fix-20261006/validation.json) and [SHA-256](builds/cusa04301-tweak-menu-blur-fix-20261006/SHA256SUMS.txt)
- [Focused source patch](patches/dreams-tweak-menu-blur-fix-20261006.patch)
- [Cumulative main-source diagnostic snapshot](patches/dreams-diagnostic-source-20261006-tweak-menu-blur.patch)

Place the EXE and launch script beside your existing runtime dependencies. Launch through the script, passing the normal game arguments, or start your launcher from an environment with `SHADPS4_DREAMS_TEST_GLASS_MIP_COORDS=1`. Launching the EXE directly without this setting leaves this specific correction off. Restart the emulator when changing the setting; unset it or use `0` to compare the original behavior. Use Precise GPU readbacks. No game files or raw proprietary shader dumps are included.

This is the exact diagnostic executable tested locally, not a complete portable runtime. The cumulative snapshot contains earlier experimental changes as well as this fix; the focused patch isolates this correction. Retain the separately documented external-submodule patches when building from source. EXE SHA-256: `9a17716a4787a3edb23a68ec1be756463f13a703e3a046d30a6fe5bf05f8508c`.
