# Dreams ordered-count offline replay

This directory contains a standalone Python model of the Dreams sculpt visibility chain. It does
not link to shadPS4, write emulator state, or start Dreams.

## What is proved offline

`ordered_replay.py` models the decoded guest operations, not a count-only approximation:

- `d8b4ddb5`: ten stable, one-bit radix partitions. For every pass it calculates the guest output
  index for zero and one lanes and rejects holes, slot collisions, out-of-range writes, wrong
  counters, output changes, and loss of equal-key stability.
- `7aa925e9`: `selector >> 10`, dword-addressed U32x2 lookup, two ordered reservations, first stream
  seeded at zero, and the second stream seeded at the active input count.
- `016b9f6a`: the same two-category reservation with direct U32x2 inputs.
- downstream checks: invalid low-24-bit references, unexpected duplicate references, expected draw
  count, optional decoded spatial points, and complete captured VS370 positions.

The candidate set is finite. Its alternatives are only the implementation ambiguities seen in this
investigation: guest dword versus byte/descriptor-element addressing, workgroup creation versus
host completion order, lane-prefix order, GDS counter seeding, and stable versus unstable equal-key
ordering. It does not search arbitrary constants.

Run all deterministic regressions:

```powershell
python -m unittest discover -s tools/tests/dreams_ordered_replay -p 'test_*.py' -v
```

The low-level `test_ordered_count_semantics.py` suite runs without Vulkan and separately locks down
the DS control bits, M0/byte-to-dword counter selection, logical-ticket ordering (including the
observed 11-bit token wrap), the host pass's exclusive-prefix recurrence, and x-fast flattening of
non-cubic 3D dispatches.

Run the three synthetic chains directly:

```powershell
python tools/tests/dreams_ordered_replay/ordered_replay.py synthetic --shader d8
python tools/tests/dreams_ordered_replay/ordered_replay.py synthetic --shader 7aa
python tools/tests/dreams_ordered_replay/ordered_replay.py synthetic --shader 016
```

Each fixture has 137 records across three wave64 workgroups, repeated radix keys, mixed final-list
flags, and an intentionally different host completion order. The exact guest interpretation is the
only candidate that passes every invariant. In particular, the d8 completion-order candidate keeps
all ten final counters correct while still creating hundreds of holes. This prevents a count match
from being mistaken for a correct implementation.

## Existing capture inventory

Run:

```powershell
python tools/tests/dreams_ordered_replay/ordered_replay.py inventory `
  --capture-root D:\Downloads\Dreams-shadPS4-Fix\isolated-sphere-test-20260819
```

The current inventory contains:

- one visibility-list summary log with a 175-record dispatch and the first eight selector/lookup/
  output records;
- eight complete VS370 occurrences with four instances each, including list records, object keys,
  indirect arguments, indices, all post-VS float4 attributes, and per-vertex validity;
- one complete 897-command `90272fc4` count-chain dump.

It does **not** contain one aligned d8 → 7aa/016 → VS370 dispatch. Therefore the harness reports
`full_real_chain_candidate_selection: false`. The current data is enough to regression-test the
finite semantics and inspect real post-VS data, but not enough to choose a real-chain winner.

The exact missing aligned files are listed in `CAPTURE_FORMAT.md` and in the inventory JSON. The
harness deliberately stops at “insufficient” instead of filling those gaps with guessed geometry.

Inspect a real post-VS occurrence without opening a window:

```powershell
python tools/tests/dreams_ordered_replay/ordered_replay.py vs370 `
  D:\Downloads\Dreams-shadPS4-Fix\isolated-sphere-test-20260819\captures\vs370-interface-20260822-203633\occ00
```

The current `occ00` has 32 finite vertices, zero invalid vertices, zero degenerate projected
instances, and six finite reference-field interpretations. Those facts constrain the downstream
mapping, but fixed post-VS positions cannot visually distinguish upstream ordered-count candidates.

## Interactive viewer honesty rule

`viewer.py` has Yes and No controls, but the visual-selection mode must only be used with an aligned
capture that lets the harness replay a candidate and map every output record to captured geometry.
The existing VS-only occurrences do not meet that requirement: changing a label while drawing the
same positions would be a fake visual test. For those captures, `--list` is diagnostic only. The
interactive path reports each exact missing buffer and does not open a misleading window.

When an aligned capture is available, No advances to exactly one next reconstruction. Yes writes a
JSON record containing the chosen semantics, input hashes, scope, and proof limits, then stops.

## Automatic depth-shape ranking

After exact Vulkan replay writes one bundle per ordered candidate, rank the candidates without
looking at fragment colors:

```powershell
python tools/tests/dreams_ordered_replay/depth_shape_score.py D:\captures\candidate-run
```

The default layout is `candidate-run/<candidate>/replay-depth.bin`, with `pre-depth.bin` and
`manifest.tsv` either shared at `candidate-run` or duplicated identically in every candidate
bundle. Use `--glob` for a flat output naming scheme and `--json` for machine-readable results.
The ranker compares raw D32Sfloat bits against the captured pre-draw depth, then scores the largest
8-connected component, disconnected pixels, enclosed holes, convex fill, and a four/six-edge
cuboid-like outline. Ordered-output validity remains a hard filter before shape scoring; a visually
compact result cannot excuse counter holes, collisions, invalid references, or GPU address faults.

## Guest/source evidence used by the model

At the time this harness was added:

- `src/shader_recompiler/dreams_compat.h:190` documents d8's ten pass counters at GDS dwords
  341–350; lines 214–232 define the two ordered operations and counters for 7aa/016.
- `cs_0x00000000d8b4ddb5.irprogram.txt:28-30` selects the pass bit; lines 48–81 contain the ordered
  prefix and the zero/one output-index equations.
- `cs_0x000000007aa925e9.irprogram.txt:17` shifts the selector by ten; lines 43 and 59 reserve GDS
  351/352; lines 95 and 125 store the two output categories.
- `cs_0x00000000016b9f6a.irprogram.txt:39` and line 55 reserve GDS 353/354.
- `before-vs370-bda-prewarm-20260821-172024-shad_log.txt:15625-15641` records the 175-item 7aa
  example: sampled selector mapping is valid, GDS 351 changes 0→175, GDS 352 remains 175, all 175
  output records change, the draw consumes 175 instances, and downstream invalid lookup IDs are 0.

These are evidence references, not runtime dependencies. The tests use Python's standard library
only.
