# Tren collision correction — October 29, 2026

**[Download the graphical launcher package](https://github.com/JesusDaBest/Dreams-ShadPS4/raw/refs/heads/main/builds/cusa04301-tren-collision-20261029/Dreams-graphical-launcher-tren-collision-20261029.zip)**

Extract the ZIP, open **shadPS4QtLauncher.exe**, add Dreams and click **Play**.

This fixes a shader arithmetic error that could corrupt a collision result and make the train disappear in Tren. The legacy reciprocal instruction now handles zero and sign bits correctly instead of converting infinity to an integer. The train stayed visible through both reported failure locations in the corrected build.

It also includes the tested mixed-format texture-copy correction already present in that build: copies between compressed and uncompressed images are bounded by the destination's block capacity. All [previous rendering fixes](COMPRESSED_COPY_20261029.md) are retained.

Core SHA-256: `5e85d6f74346aa3bd8797fe41a20221ac58425c2eaa6ac7c6133d72692ae3bec`.

## Verification

- 4,096 production-compiler reciprocal cases and eight complete collision-shader cases pass without validation errors. Six collision cases produced invalid results before the correction; none do afterward. Two unaffected cases are byte-identical. Seven saved shader comparisons remain unchanged.
- A three-minute live Tren check recorded 3,390 stable position samples with no invalid train transforms. The running collision shader matched the GPU-tested shader, and both reported disappearance locations passed.
- The mixed-format correction passed 4,368 GPU cases and 2,952 compressed-copy regression cases; another 2,408 legal controls passed on the prior copy code. The CPU range check covers 8,912 regions, preserving 6,656 already-valid regions.
- The clean release uses the tested production objects with five custom diagnostic objects removed. Both graphical and direct launch routes passed verification without starting another game. The clean executable was not separately driven through Tren.

Testing used AMD Radeon 8060S. Other physical GPUs remain untested, and no performance improvement is claimed. The collision GPU cases use finite synthetic inputs reproducing the captured failure pattern; not all exact event-time inputs were captured. The copy matrix excludes 48 valid-input selections with overlong expanded mip chains, a separate allocation issue still under investigation. No specific live artifact is attributed to that copy correction.

## Download and source

The graphical launcher, matching settings and optional tutorial-skip tool are included. The package contains no games, firmware, saves or creations.

- [Checksums](../../builds/cusa04301-tren-collision-20261029/SHA256SUMS.txt), [verification](../../builds/cusa04301-tren-collision-20261029/verification.json), [runtime manifest](../../builds/cusa04301-tren-collision-20261029/runtime-manifest.json).
- [Source manifest](../../builds/cusa04301-tren-collision-20261029/source-manifest.json) and [new patch](../../patches/dreams-tren-collision-20261029.patch). Apply all patches in order from the public base. Both changed source files were reconstructed and verified against that stack; a fresh full-source rebuild is not claimed.
- [Launcher source](../../builds/cusa04301-tren-collision-20261029/DreamsCore.cs), [tutorial helper](../../builds/cusa04301-tren-collision-20261029/SkipDreamsTutorial.cs), unchanged [Qt launcher source](../../builds/cusa04301-image-cache-20261021/qt-launcher-source-99e9829.tar.gz).

All 45 package files are allowlisted and hash-verified. The [previous package](../../builds/cusa04301-compressed-copy-20261029/) remains available.
