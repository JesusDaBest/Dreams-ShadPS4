# Continue Development

For applying and compiling the retained source fixes, start with [SOURCE_BUILD.md](SOURCE_BUILD.md). The working local profile uses **Precise readbacks**. The active game metadata reports CUSA04301 **VERSION 02.64** and **APP_VER 01.00**; APP_VER alone must not be treated as the content update version. Game update 2.65 remains unverified.

> **Latest September 4 runtime test:** [CLEAN_CORE_TEST_20260904.md](CLEAN_CORE_TEST_20260904.md) records two actual official-core runs with the general GPU corrections. Both fail translating `DS_ORDERED_COUNT` in `cbac06d2`; Dreams is not working on this build. This supersedes the earlier plan to establish a clean-core run. Earlier GPU provenance and correction tests remain in [HANDOFF_20260904.md](HANDOFF_20260904.md).

## September 2 continuation state

Read [HANDOFF_20260902.md](HANDOFF_20260902.md) before using the older checkpoint workflow below.
The August 29 source and executable remain the last preserved visual checkpoint. The complete
September 2 experimental source is preserved separately as:

```text
branch: codex/wip-native-doc-addressing-20260902
commit: f553e23
patch:  patches/0001-WIP-preserve-September-2-Dreams-ordered-count-invest.patch
sha256: BE947C77B4A71BFBA2581BBA877ABC16BE60CCF458355340ABA0AE05D5E68108
```

That WIP includes valuable bounded diagnostics and exact replay work, but also an unverified x4
ordered-counter address experiment. The user experienced severe edit-mode lag in that build, but
causality is unresolved. Do not treat it as the known-good build or merge it wholesale as a fix.

The immediate proof-producing task is to resolve the Liverpool address unit from official AMD
documentation and capture the producer bound at flattened SRT word 41. Keep the diagnostic source
while excluding the unverified x4 family for the first safe rerun.

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

## Historical August 29 downstream root-cause order

Use this order only after the upstream producer count/prefix is proven complete. The September 2
zero-instance capture makes the producer-count test above the current first task.

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
