# Continue Development

## Exact source state

Start from upstream shadPS4 commit:

```text
555c458c9fdd33cb4686492374519c7bb112a891
```

Apply both patches from this repository:

```bash
git apply --check /path/to/Dreams-ShadPS4/patches/dreams-focused-20260829-full-covered-filled.patch
git apply /path/to/Dreams-ShadPS4/patches/dreams-focused-20260829-full-covered-filled.patch
git -C externals/sirit apply --check /path/to/Dreams-ShadPS4/patches/sirit-group-nonuniform-shuffle-20260829.patch
git -C externals/sirit apply /path/to/Dreams-ShadPS4/patches/sirit-group-nonuniform-shuffle-20260829.patch
```

Patch hashes:

```text
CCD22A4B97E246A62FAFA71DFE40894DDA9C90DD52A6E020F583512AE30B29C4  dreams-focused-20260829-full-covered-filled.patch
0F826E81BB003AD6A797F942CA6B3F51BDBBFB8C189B21B1C4885E6FEDBE2041  sirit-group-nonuniform-shuffle-20260829.patch
```

Both patches were reapplied successfully in clean detached worktrees at the stated revisions.

## Known-good binary

The exact user-validated executable is included at:

```text
builds/cusa04301-full-covered-filled-20260829/shadps4.exe
```

Its SHA-256 is:

```text
183D9914A395D8AD804428B418E13474D71172A6146386DD9E7D159F26AE14CC
```

It is a checkpoint for comparison, not a claim that Dreams is fixed.

## Build and test

Use the normal shadPS4 Windows build setup. Keep compilation parallelism conservative on the test
machine; the validated Release build was produced successfully with one compile job.

The repository's three offline Dreams tool suites should be run from their own directories:

```text
tools/tests/dreams_d25_shape_finder
tools/tests/dreams_exact_replay_viewer
tools/tests/dreams_ordered_replay
```

Run `python -m unittest -v` in each. The checkpoint result was 68 passes plus one optional
real-capture skip.

## Current root-cause order

1. Capture `0xce3b8413` sample coordinates, both sampled values, exact atlas neighborhoods, and its
   discard decision for the known cube draw.
2. If samples differ from software interpolation, inspect image view, sampler mode, or
   synchronization.
3. If samples match but discard differs, inspect wave/EXEC/discard lowering.
4. If both match, inspect VS370 parameters, object association, and LOD.
5. Treat the fullscreen DCC decoder as the next target for material/color errors, not as the first
   explanation for the panel silhouette.

The A3 BDA-prewarm hypothesis has already been falsified by `registered_before=true`; do not restore
that experiment without new contradictory evidence.

## Development discipline

- Keep the `0x4ebeffd2` exact ordered replay and its `_doc1` cache isolation intact.
- Change one rendering variable at a time.
- Challenge each conclusion with a falsifier before changing code.
- Keep one saved scene and camera position for comparisons.
- Record executable, source, cache, and capture identity for every test.
- Treat nonzero draw commands and fuller geometry as evidence, not final correctness.
- Disable broad diagnostics for performance measurements.
- Preserve the included binary before deploying any new experiment.

## Isolation

The snapshot includes emulator-wide changes and has not been regression-tested across other games.
Use a dedicated folder and portable `user` directory for `CUSA04301`, and preserve save backups.
