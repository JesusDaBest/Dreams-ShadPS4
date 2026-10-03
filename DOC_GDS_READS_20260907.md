# Shared DOC: fixed scalar GDS reads

Added generic support for ordinary scalar U32 reads of fixed guest GDS words
around the shared DOC prototype. Reads must use the same binding as DOC and
remain disjoint from every DOC counter, including later reservations. Values
come from the live GDS buffer. The planner exports sorted, unique
`stable_gds_words`; their stability across collect, prefix and replay remains
a caller obligation. This does not measure PS4 ordering or exclude external
writes by itself.

All ordinary GDS writes/atomics, non-U32 reads, dynamic/out-of-range/private
addresses, different bindings and counter overlap remain rejected. Prefix
side effects and dependencies on earlier DOC results retain their checks.
No shader-hash exception or captured-value substitution was added.

## Verified result

- 105 synthetic GPU cases passed, including 22 new cases covering 1,2 and 4 DOCs.
- 126 planner checks passed;13 omitted-prefix controls detected failures and
  five invalid-phase controls preserved every buffer.
- All 15 fixture modules and the prefix module passed Vulkan 1.3 SPIR-V validation.
- Zero mismatches across 104,160,366 compared dwords and zero Vulkan errors.
- All 26 recovered actual-shader permutations completed offline compiler replay.
- Four actual shaders emitted validated SPIR-V: `252ccde6`, `6f408bbc`,
  `a25580f7`, and `fff20d00`, up from two before this change.
- `6f408bbc` reads GDS 198 and reserves counters 228–231. `a25580f7` reads
  GDS 199 and reserves counters 232–235. Each newly accepted shader has four DOCs.

The GPU fixtures use changing live GDS values and compare all buffers after
each phase, including guards and unchanged guest words. Existing1/2-DOC cases
retain their earlier memory allocation. The fixture build reused 62 units and
compiled 22 in 50.297 seconds; replay reused 79 objects and compiled one unit in
31.203 seconds. Both used at most two BelowNormal workers. Fixture execution
took 5.521 seconds including setup/readback/checking; this is not a game benchmark.
Thirty-nine pre-existing OBS/overlay loader warnings were recorded.

## Remaining work

The remaining 22 recovered permutations first reject on nonconstant DOC
counters(5), UndefU32(5), workgroup shape(4), ordinary GDS writes(3),
loops/early exits(2), image sampling(2), or image dimension queries(1).
These are first failures, not the full list of requirements per shader.
Four of 26 known DOC shader bodies still lack cached input metadata.

The next bounded candidate is constant normalization for the counter expressions
in `016b9f6a` and `7aa925e9`. Their recorded expressions appear reducible to
fixed counters, but that simplification still needs implementation and proof;
no captured runtime value should be silently baked into generic shader code.
Other GDS cases include dynamic addresses or guest-visible writes and require
separate analysis.

No actual Dreams shader was GPU-executed in this milestone. The normal renderer
does not enable the experimental profile. Full DOC support, gameplay improvement
and full performance remain unverified. The installed best emulator still hashes
to `183D9914A395D8AD804428B418E13474D71172A6146386DD9E7D159F26AE14CC`.

## Evidence

- GPU source and results (`D:/CodexData/Workspaces/2026-09-04/usi/work/doc-shared-gpu/README.md`; local evidence)
- Actual shader compiler replay (`D:/CodexData/Workspaces/2026-09-04/usi/work/doc-input-capture/README.md`; local evidence)
- Isolated source: `D:/CodexData/Worktrees/Dreams-ShadPS4/doc-shared-20260907`
- Earlier milestone: [DOC_FLAT_READS_20260907.md](DOC_FLAT_READS_20260907.md)

The GPU and replay directories preserve their preceding two-shader/83-case
baseline reports, manifests and emitted modules. Cache identity and recovered
profile association retain the provenance limitations described in the replay
README; compilation agreement does not turn those inputs into a PS4 oracle.


Publication note: local evidence paths record the original investigation environment. They are not downloadable repository links. Test results above are historical reports, not newly rerun validation.
