# DOC-prefix update — November 12, 2026

**[Download the graphical package](https://github.com/JesusDaBest/Dreams-ShadPS4/raw/refs/heads/main/builds/cusa04301-doc-prefix-20261112/Dreams-graphical-launcher-doc-prefix-20261112.zip)**

Extract the ZIP and open **shadPS4QtLauncher.exe**. No commands or extra setup.

DOC prefix calculation now uses parallel GPU work while preserving guest allocation order and uint32 counter wraparound. Collect/replay remains in place. All previous fixes and shader preloading are retained.

Verification: 720 prefix cases, 468 layout cases, 1,056 full-shader comparisons and 184 interleaved full-path comparisons passed with no Vulkan validation errors. The same prefix object is used in the tested private build and clean release.

On Radeon 8060S at the owner's 20 W limit, a synthetic 256-group full-path test fell from 162.62 to 45.36 microseconds; 24 groups fell from 44.84 to 39.32. Eight groups increased slightly, from 38.44 to 39.78. These are shader-fixture timings, not measured game FPS gains or native PS4 comparisons.

The owner reported correct visuals and the best performance so far in the private build. A stationary Tren sample measured about 21 render FPS / 22.6 game FPS, without a matched previous-build sample. This does not replace the separate heaviest-scene result on the front page. Native PS4 DOC performance parity has not been established.

The release contains no custom debug tools, authoring connection, games, or user saves. The normal graphical launcher, FPS toggle and optional tutorial-skip helper remain.

Source: [patch](../../patches/dreams-doc-prefix-20261112.patch) · [complete source manifest](../../builds/cusa04301-doc-prefix-20261112/source-manifest.json) · [previous update](SINGLE_MIP_WRITE_20261102.md).
