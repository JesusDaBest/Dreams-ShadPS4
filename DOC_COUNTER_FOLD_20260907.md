# DOC counter expression simplification

A general compiler change now recognizes fixed counter addresses hidden by
bit operations. It folds `(C | x) >> s` when unsigned extraction bounds or an
immediate AND mask prove that all possible bits of `x` are shifted out. It
preserves the original dynamic expression for other uses. No captured values,
game hashes, or assumed GPU behavior are involved.

In `016b9f6a` and `7aa925e9`, `x` is an unsigned 11-bit extraction. It cannot
affect bits 16 and above. The ordinary compiler pass consequently reduces the
DOC addresses to 353/354 and 351/352 respectively. Both shaders now compile
through the shared DOC prototype, bringing coverage to **6 of 26 tested DOC
permutations**, up from 4. This is not a percentage of overall Dreams correctness.

Verified:

- 117 synthetic GPU cases passed, including 12 new cases with live varying
  inputs and dynamic token values retained in the output.
- 126 planner checks and 48 optimizer checks passed, with 159,179 unsigned
  numerical comparisons, including every 11-bit token for the target formulas.
- All 17 fixture modules plus the prefix module passed Vulkan 1.3 validation.
- Fifteen omitted-prefix controls detected failures; five invalid-phase
  controls preserved every buffer.
- Zero mismatches across 114,417,414 compared dwords and zero Vulkan errors.
- All 26 actual-shader compiler replays completed; six emitted validated SPIR-V.

Tests retain changing counters where bits survive the shift, signed extraction,
variable/invalid shifts, invalid extraction bounds, and addition that can carry.
The three remaining counter-address failures still depend on changing inputs.
The new fold correctly leaves those selectors dynamic.

The GPU fixture build compiled five units and reused 79 in 36.140 seconds;
replay needed only relinking and completed in 21.234 seconds. Builds used at most
two BelowNormal workers. Fixture process time 6.496 seconds includes setup,
readback and checking; it does not measure game performance. Forty-five
OBS/overlay loader warnings were recorded.

The other blockers remain memory access/order, workgroup shape, control flow,
and image operations. A read-only audit also found that the five UndefU32-first
records use placeholders in conditional SSA joins, with observability still
unproven in several cross-lane cases. All five have independent memory or image
blockers. Assigning zero or merely allowing Undef would not establish support.

No actual Dreams shader was GPU-executed, and the normal renderer still leaves
the experimental DOC path disabled. No gameplay or full-performance improvement
is verified. The installed best emulator remains unchanged, SHA256
`183D9914A395D8AD804428B418E13474D71172A6146386DD9E7D159F26AE14CC`.

- [Standalone production patch](patches/generic-shift-constant-fold-20260907.patch)
- GPU tests and provenance (`D:/CodexData/Workspaces/2026-09-04/usi/work/doc-shared-gpu/README.md`; local evidence)
- Actual compiler replay (`D:/CodexData/Workspaces/2026-09-04/usi/work/doc-input-capture/README.md`; local evidence)
- [Previous milestone](DOC_GDS_READS_20260907.md)

Both test directories preserve preceding results, manifests and emitted modules
in `baseline-before-counter-fold-20260907/`; the GPU directory also preserves
the previous fixture sources. The production patch changes only
`constant_propagation_pass.cpp`; it is not a complete emulator build or DOC fix.


Publication note: local evidence paths record the original investigation environment. They are not downloadable repository links. Test results above are historical reports, not newly rerun validation.
