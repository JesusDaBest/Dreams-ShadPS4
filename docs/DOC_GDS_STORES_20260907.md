# DOC suffix GDS stores

The shared DOC prototype now supports scalar `StoreBufferU32` operations to
constant guest GDS words after the final reservation. Stores must use the DOC
GDS binding, stay below word 16384, and avoid every DOC counter and every ordinary
GDS read in the shader. Values and suffix conditions may depend on DOC results.
The planner records sorted, unique `written_gds_words` separately from
`stable_gds_words`. Earlier stores, dynamic addresses, atomics, and other store
widths remain rejected.

Verified in the standalone fixture:

- **133 synthetic GPU cases passed**, including 16 suffix-store cases, with
  zero mismatches across **128,093,478 compared dwords**. The fixture ran on
  Radeon 8060S with 24 groups of 64 lanes.
- **152 planner checks** and **48 optimizer checks** passed, including
  **159,179 unsigned comparisons**.
- **20 SPIR-V modules** passed Vulkan 1.3 validation before GPU execution.
- All **24 controls** behaved as expected: 17 omitted-prefix controls detected
  failures; seven invalid-phase controls preserved the buffers.
- Zero Vulkan validation errors; 51 OBS/overlay loader warnings.

Evidence: GPU results (`D:/CodexData/Workspaces/2026-09-04/usi/work/doc-shared-gpu/results.json`; local evidence),
run provenance (`D:/CodexData/Workspaces/2026-09-04/usi/work/doc-shared-gpu/run-provenance.json`; local evidence),
and offline check log (`D:/CodexData/Workspaces/2026-09-04/usi/work/doc-shared-gpu/emit.log`; local evidence).

All 26 recovered actual-shader permutations completed offline compiler replay.
**Eight of 26** now produce validated shared-DOC SPIR-V, representing **seven
distinct shader hashes**. The increase from six permutations is `81cf58cf`
permutations 0 and 1: both record the suffix output at GDS word 771. This adds
one distinct shader, not two. These modules were compiled and validated only.
See replay results (`D:/CodexData/Workspaces/2026-09-04/usi/work/doc-input-capture/replay-results.json`; local evidence)
and the [preceding counter-fold milestone](DOC_COUNTER_FOLD_20260907.md).

The fixture build (`D:/CodexData/Workspaces/2026-09-04/usi/work/doc-shared-gpu/build-provenance.json`; local evidence)
compiled 22 units and reused 62 in 43.578 seconds. The
replay build (`D:/CodexData/Workspaces/2026-09-04/usi/work/doc-input-capture/build-provenance.json`; local evidence)
compiled one unit in 24.078 seconds. Both used at most two BelowNormal workers.
The GPU fixture process took 6.249 seconds including setup, readback and checks;
this is not a gameplay performance measurement.

The caller must still establish unique runtime writers, stable and current read
inputs, output ownership until replay finishes, and the required ordering and
barriers. Static acceptance does not prove those conditions. No actual Dreams
shader was GPU-executed, and this milestone adds no renderer scheduling or
gameplay integration. No gameplay correctness or performance improvement is
verified. The installed best emulator remains unchanged, SHA256
`183D9914A395D8AD804428B418E13474D71172A6146386DD9E7D159F26AE14CC`.


Publication note: local evidence paths record the original investigation environment. They are not downloadable repository links. Test results above are historical reports, not newly rerun validation.
