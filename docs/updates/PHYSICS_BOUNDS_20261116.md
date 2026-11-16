# Physics bounds — November 16, 2026

**[Download the graphical package](https://github.com/JesusDaBest/Dreams-ShadPS4/raw/refs/heads/main/builds/cusa04301-physics-bounds-20261116/Dreams-graphical-launcher-physics-bounds-20261116.zip)**

Extract the entire ZIP and open **shadPS4QtLauncher.exe**.

Includes the promoted GPU alias-proof/ticket DOC paths and bounded write tracking for Predict and ApplyConstraints. Exact shader, launch and descriptor checks retain full-range handling whenever a bound cannot be proved. Shader calculations and earlier fixes are unchanged.

In the owner's repeated wall-break test, the reported dip was about **24 FPS**, versus about **17 FPS** before. Recorded frames over 50 ms were 20 versus 70 across two 30-second windows. These were separate dynamic runs, not a controlled speedup ratio; other physics slowdowns remain.

Both physics kernels and all four output bounds activated in the private build. The captured shader modules passed 280 GPU cases; the clean bounds passed 5,211 guard checks. Both packaged launcher routes passed verification without starting a game. Private debug/capture/measurement tools are excluded. The broader bounds audit is not part of this release.

Source: [patch](../../patches/dreams-gpu-proof-physics-bounds-20261116.patch) · [complete source manifest](../../builds/cusa04301-physics-bounds-20261116/source-manifest.json) · [previous update](ORDERED_GUARDS_20261113.md).
