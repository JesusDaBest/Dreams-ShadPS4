# Shared DOC implementation: September 7 checkpoint

Later implementation results are in [DOC_FLAT_READS_20260907.md](DOC_FLAT_READS_20260907.md).

The current work is an isolated shared collect/prefix/replay prototype, separate
from the installed smooth Dreams build. It is not a complete DS_ORDERED_COUNT
implementation and has not produced a verified in-game improvement.

Earlier synthetic validation passed 72 GPU cases and 68 planner checks. The latest
milestone recovered historical compiler inputs for 22 of the 26 DOC shaders in
the installed Dreams shader pack, avoiding a new emulator capture for those
compiler checks.

All 26 recovered C3 permutations completed offline resource-tracked compilation.
Their regenerated resource-table walker bytes, flattened userdata, runtime
descriptor recipes and checked buffer fields matched the saved metadata. Only
one permutation currently passes the shared planner's static restrictions.
Remaining first rejections concern constant reads, undefined IR values, extra
GDS accesses, workgroup shapes, and control flow. These are not an exhaustive list
of the work remaining, and static acceptance is not hardware correctness.

The complete report, scripts, results and provenance are in
the offline replay folder (`D:/CodexData/Workspaces/2026-09-04/usi/work/doc-input-capture/README.md`; local evidence).
The modified emulator source is
`D:/CodexData/Worktrees/Dreams-ShadPS4/doc-shared-20260907`.

The next implementation target is stable flattened constant reads, tested across
the recovered shaders. Full GPU replay still needs actual resource contents and
dispatch/counter state. Metadata alone cannot establish PS4 ordering semantics.

The installed best executable remains unchanged, SHA-256
`183D9914A395D8AD804428B418E13474D71172A6146386DD9E7D159F26AE14CC`.
No emulator launch or driver change occurred in this milestone.


Publication note: local evidence paths record the original investigation environment. They are not downloadable repository links. Test results above are historical reports, not newly rerun validation.
