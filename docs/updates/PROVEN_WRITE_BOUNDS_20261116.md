# Guarded write bounds — November 16, 2026

**[Download the graphical package](https://github.com/JesusDaBest/Dreams-ShadPS4/raw/refs/heads/main/builds/cusa04301-proven-write-bounds-20261116/Dreams-graphical-launcher-proven-write-bounds-20261116.zip)**

Extract the entire ZIP and open **shadPS4QtLauncher.exe**.

Adds actual-SPIR-V write-footprint analysis for 58 audited ordinary compute kernels, covering 85 potentially bounded output bindings. Live verification observed 27 kernels and 40 bindings, including all six CenterOfMass outputs. Unknown addresses/effects, overflow, unsupported layouts, image aliases and unsafe dispatch conditions retain full tracking. Existing eight bounds, ordered/DOC paths, input preparation and shader calculations are unchanged.

645 real-GPU cases passed on 43 unchanged captured modules with synthetic payloads; 15 candidates have static proof/guards but no GPU exercise by that harness. The clean analyzer passed 420,481 arithmetic/address/cache/context checks. Both packaged launcher routes were verified without starting a game. Private debug, capture and comparison controls are excluded.

**No additional FPS gain is established.** The intact-wall same-process comparison stayed near 30 FPS in both modes. In separate wall-break runs, the owner reported a 20 FPS dip with quick recovery for this broader build, versus 24 FPS for the preceding two-kernel build; the broader run recorded more slow frames. Those dynamic runs do not establish a controlled cause. The [previous build](PHYSICS_BOUNDS_20261116.md) remains available.

Source: [patch](../../patches/dreams-proven-write-bounds-20261116.patch) · [complete source manifest](../../builds/cusa04301-proven-write-bounds-20261116/source-manifest.json) · [analyzer tests](../../builds/cusa04301-proven-write-bounds-20261116/proven-write-bounds-tests.cpp). Apply the manifest's patches in order; full fresh-build reproducibility is not established. The tests use C++20, the renderer_vulkan include directory and the SPIR-V headers from the source's Sirit submodule.
