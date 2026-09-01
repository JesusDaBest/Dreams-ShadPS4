# Dreams sculpt-rendering checkpoint — 2026-08-29

## What is validated

- Dreams `CUSA04301` reaches the edit-mode test scene at 30 FPS in the recorded view.
- The test sculpt is covered and filled on all visible sides instead of losing large chunks.
- Sculpt and tweak-menu jitter was absent in the validated run.
- Homespace lighting and the edit floor were normal in the validated run.
- The executable in this directory is the exact binary used for that validation.
- A clean rebuild after restoring the source differed from the validated executable at only two PE metadata bytes; file size and executable code were otherwise identical.

This is meaningful progress, but it is not the final visual fix. The sculpt is still rendered as a regular grid of rounded panels instead of the intended Dreams fleck surface.

## Confirmed root cause fixed at this checkpoint

Compute shader `0x4ebeffd2` uses PS4 `ds_ordered_count` semantics while writing the B1 seed stream. A native Vulkan atomic allocates rows in host workgroup-arrival order, which is nondeterministic and does not preserve the guest's logical workgroup order.

The retained implementation performs:

1. one collect write per guest logical workgroup;
2. an exclusive prefix pass in guest token order;
3. replay of the guest writes at those exact ordered offsets; and
4. publication of the final guest counter value at GDS dword `0x79`.

The validated run logged the direct collect/prefix/replay path with 64 logical groups and no fallback, Vulkan validation, device-loss, or out-of-memory error. The shader cache revision is isolated with the `_doc1` suffix.

This is emulator correctness, not a visual replacement: it reproduces the ordering behavior the PS4 shader requested.

## Evidence that narrowed the remaining tiled-surface defect

### Rejected: missing A3 dynamic-constant residency

An existing A3 (`0xa3a9e9ef`) BDA prewarm was enabled for one falsification run. Its first result reported `registered_before=true` and `registered_after=true`. The needed range was already resident, so the prewarm hypothesis was rejected and the experiment was reverted. It is not part of this checkpoint.

### Rejected as the leading cause: `0x63ddac84` table handoff/order

Existing captures show the `0x63ddac84 -> 0xa3a9e9ef` descriptor handoff is coherent:

- producer and consumer count tables match;
- producer and consumer record tables match;
- focus probes have no overflow or comparison mismatch; and
- sampled A3 lookup outputs match the later B1 consumer values.

This does not prove every input is perfect, but it makes that handoff a lower-probability cause than the final sculpt-sampling path.

### Current highest-probability subsystem

The visible sculpt path is:

`ordered sculpt records -> VS 0x3706083c cuboid proxies -> FS 0xce3b8413 atlas raymarch/coverage/depth -> fullscreen VS 0xa33ab236 + FS 0xdcc325c2 decoder -> lighting`

The draw uses a fixed 14-index cuboid strip per instance. Captured atlas IDs are unique per instance, and the corresponding 2048x2048x128 R8 atlas bricks are nontrivial rather than empty or solid. Because the wrong result is a panel-like silhouette, the next target is the `0xce3b8413` sample/raymarch/discard stage. The later DCC pass is more likely to explain material or color errors than the panel silhouette itself.

## Next proof-producing diagnostic

Use the exact checkpoint executable with only a fresh `SHADPS4_DREAMS_CE3_FLECK_CAPTURE_DIR` set. Once the known cube and camera view are visible, create `capture.request` in that directory. The trace records both sample sites, atlas neighborhoods, and the discard condition for the selected draw.

Interpretation:

- shader sample differs from captured atlas interpolation: inspect image view, sampler mode, or synchronization;
- samples match but discard is wrong: inspect wave/EXEC/discard lowering;
- samples and discard both match: move upstream to VS370 parameters, object association, or LOD selection.

Do not enable the unrelated gather-focus trace; it is not required and can add the blue diagnostic outline. The current ReadConst trace auto-captures the first qualifying draw after launch, so it must not be treated as paired with the manually triggered cube trace unless that cube is the first qualifying draw.

## 2026-09-01 follow-up: the box silhouette is produced before the final discard

Later captures completed the diagnostic proposed above and narrowed the remaining defect further.

### Builder and input evidence

- Thirty-two focused `0x62a348b1` builder captures were exact legitimate no-ops: every captured input independently predicts zero emitted records. They are not evidence that the builder silently dropped nonzero work.
- The CPU-side model configurations for the eleven stamped primitive types are distinct, and many of their computed bounds/results differ. The primitives therefore have not already collapsed into one cube configuration at that boundary.
- `0x8b19605c` is a spatial replay-list compactor. Its guest ISA contains no primitive evaluator or cube/sphere/cylinder switch, and its current SPIR-V validates. It is not a plausible direct source of the common cube shape.

### Exact CE3 sampler evidence

The fresh capture at `6fd0d52-ce3-fleck-20260901-0302` selected Param1 `0x420077ab`, reached both static sample sites, and recorded the terminal discard path. Both shader samples agree with the captured R8 atlas bytes and the configured interpolation within expected UNORM quantization. This rejects image view, sampler interpolation, and stale sampled-image contents as the leading cause for that draw. It does not prove that the upstream-generated atlas values encode the intended primitive.

### Exact single-ID discard evidence

The first coverage run selected Param1 `0x42007961` and counted 118 initial samples. Exactly 111 reached the export path and seven reached the guest's unconditional reject path; the conditional discard was false for all 111 exporting invocations. The terminal accounting was exact (`118 = 111 + 7`).

### Complete multi-ID edit-mode draw

Commit `d9317ff` enlarged the collision-detecting coverage inventory to 4,096 slots with eight straight-line probes. The complete capture at `d9317ff-ce3-coverage-20260901-0558` recorded:

- 603 distinct Param1/atlas IDs;
- 50,046 initial samples;
- 47,413 exporting fragments;
- 2,633 unconditional terminal rejects;
- zero true conditional discards;
- zero unresolved table updates; and
- exact terminal conservation (`50,046 = 47,413 + 2,633`).

Only 5.26% of sampled proxy fragments were carved away, while 94.74% survived. This directly matches the observed result in which non-cube primitives retain most of their fixed cuboid proxy. The result rules out broken terminal discard/EXEC lowering for the complete captured draw. Combined with the exact sampler result, the next boundary is the value-producing path before terminal coverage: either the upstream atlas/SDF generation encodes overly solid bricks, or CE3's ray-march arithmetic and branch decisions interpret otherwise-correct values incorrectly. The coverage table alone cannot distinguish those two possibilities, so neither is yet called the root cause.

The next proof-producing capture should classify the sampled SDF values and the major pre-terminal ray-march branches per Param1 ID while preserving the same complete accounting. That will distinguish incorrect atlas contents from incorrect CE3 numeric/control-flow interpretation without changing the rendered values.

## Verification

- Release build completed successfully with one compile job.
- `git diff --check` passed before the checkpoint commit.
- 69 offline tests ran: 68 passed and one real-capture test was skipped because its optional capture path was not supplied.
- Verified executable SHA-256: `183D9914A395D8AD804428B418E13474D71172A6146386DD9E7D159F26AE14CC`.
- Validation image SHA-256: `327C9E07B595A4E113C69ADF555F8928A804D676B0BFE912B651D822DF09ECB8`.
- Complete-coverage diagnostic executable SHA-256: `C7F9F91D06001F5C1CEC9AE5EC44B5CDCB082B3D09D7C61CB949DFC50B423AA2` (`d9317ff`).
