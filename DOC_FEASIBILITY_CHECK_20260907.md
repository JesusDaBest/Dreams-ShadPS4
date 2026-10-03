# Bounded DOC feasibility check

September 7, 2026. Further shared-planner expansion is paused. This check reviewed
four difficult shader bodies, existing specialized lowering, host integration,
and saved timing evidence. No production code changed, no build or GPU/game run
was performed, and no performance result was generated.

## Decision

The inspected hard cases do **not** demonstrate that collect/prefix/replay must
be replaced. There are concrete designs to reuse. They also do **not** establish
that completing this approach will preserve performance or finish quickly.
Do not resume open-ended compiler-coverage expansion based on this audit.
A limited runtime comparison is justified before deciding whether to scale it.
The earlier weeks-to-months estimate was a rough judgment, not a measured forecast.

## What the difficult cases actually require

- `3937a849` uses 16 waves per workgroup and four DOC sites. Its counters are
  `2304 + 4 * wave + site`: 64 streams. Its pre-DOC shared local memory transpose
  and barrier are repeatable private work; inspected external effects occur
  after all DOCs. Current shared scratch indexing would collide between waves.
  However, the research source already contains specialized 64-stream lowering
  and a matrix prefix helper. Generalization requires applying and validating
  that layout and proving memory/definedness conditions. The existing code is
  experimental and is not evidence that the installed best build uses it.
- `5ac53394` has two bounded prefix loops and one DOC after those loops, with
  stores after DOC. This points to bounded-loop analysis rather than a required
  new execution architecture.
- `f030fdc4` traverses images/buffers before DOC and performs a guest-buffer
  store before DOC. Collection must suppress that external store, and inputs
  must be proven stable and physically nonaliasing. Resource binding numbers
  alone do not prove this. There is no observed DOC-result feedback into its loop.
- `90272fc4` varies its first counter by workgroup Y. Evaluating its recorded IR
  expression for all 2,048 extracted tokens at each Y=0..3 produces counters
  167, 168, 169 and 170; the second counter is 166. Its suffix GDS destinations
  for those rows are 162..165. A fixed-counter plan cannot represent that dispatch.
  This is an 8,192-evaluation arithmetic check, not guest or GPU execution.

Source evidence: current normalized IR in
`D:/CodexData/Workspaces/2026-09-04/usi/work/doc-input-capture/ir/`;
`3937a849-p0.ir.txt:14657`, `5ac53394-p0.ir.txt:220`,
`f030fdc4-p0.ir.txt:810`, and `90272fc4-p0.ir.txt:424`.
The experimental source is
`D:/CodexData/Worktrees/Dreams-ShadPS4/doc-shared-20260907`;
specialized multi-wave lowering begins in
`src/shader_recompiler/backend/spirv/emit_spirv_atomic.cpp:767`.

## Performance and runtime decision gate

The existing direct `016b9f6a`/`7aa925e9` paths already use collect, prefix and
replay. Replacing their lowering need not increase dispatch count. More broadly,
collection can repeat payload-producing work and adds scratch traffic, barriers,
and a serial scan per counter stream. That does not imply twice the game cost.
Static instruction counts and the fixture's 6.249-second process time cannot
predict FPS. That fixture includes setup, fences, readback and CPU checking;
it has no GPU timestamp measurements.

The shared path is not connected to normal renderer scheduling. An indirect
integration also needs to publish the workgroup count expected at private GDS
word `0x184000`; the current shared emitter does not. A fast run that skips work
would fail the decision gate.

The smallest fair runtime comparison uses one source build, changing only the
eligible direct `016b9f6a`/`7aa925e9` lowering, with identical copied Precise
profiles and scene. Compare existing/shared/existing, confirm shader execution
and matching outputs/counters, then measure shader GPU times and scene frame
times using the existing profiler. Continue only if results match and costs
stay within repeated-control variation; a reproducible regression pauses expansion.
This passes or fails that family only. A separate 16-wave/64-stream fixture would
be required before widening the planner to the difficult multi-wave family.

The installed best executable is a different source checkpoint from this
experimental worktree. Comparing those binaries alone would confound the result.
No full-DOC correctness, full-performance result, or completion percentage follows
from this audit or the existing eight compiled permutations.

Previous implementation milestone: [GDS suffix stores](DOC_GDS_STORES_20260907.md).
