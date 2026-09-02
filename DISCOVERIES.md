# Detailed Discoveries

## September 2 result — empty scene originates before rasterization

A trigger-gated held-state capture produced a fully empty 3D scene while UI still rendered. The
affected indexed scene draw had `indexCount=14` and `instanceCount=0`. Immediately upstream,
queue producer `0x2bfebd3c` published indirect X = 0, so traversal shader `0xb535c6c8` dispatched
`(0,1,1)` and could not produce useful records.

This is direct evidence of an upstream producer/compaction omission. It does not explain every
dark, jittery, panelled, or cube-like render, but it rules out rasterization as the cause of this
specific empty-scene state.

The capture did not record flattened SRT words 41 and 18. Word 41 comes from SRT root + `0x64`
and bounds producer input. The existing focused producer-count refresh is therefore the next
discriminator: a zero-to-nonzero change identifies stale flattened-SRT/coherency state; a stable
zero moves the trace to the real count writer; a nonzero input with zero publication moves it to
the producer predicate and ordered-count payload.

### Ordered-counter address experiment is unresolved

The September 2 WIP changed generic and exact replay counter addresses to an x4 family, including
`0x142 -> 0x508` and `0x100 -> 0x400`. Its C++ now contradicts the committed semantic model/tests,
which require the older `/4` family. A mixed-unit run used `0x508` in 2bf but sampled only `/4`
counters, so its zero table is not decisive. The user also experienced severe lag in the full-x4
build, but no timing capture attributes that lag to address units. The `/4` interpretation is
better supported; neither has yet passed the controlled Liverpool discriminator.

The full source state and contradictory evidence are preserved in [HANDOFF_20260902.md](HANDOFF_20260902.md).

## August 29 result — full volume, remaining panel surface

This section supersedes the August 19 `DS_ORDERED_COUNT` status below while preserving that earlier
investigation as history.

### Exact ordered replay fixed a real sculpt-stream corruption

Shader `0x4ebeffd2` writes a B1 seed stream with `DS_ORDERED_COUNT`. The previous ordinary atomic
assigned output rows in host workgroup arrival order. The retained GPU path collects one result per
guest logical group, computes an exclusive prefix in guest order, replays the writes at the exact
offsets, and publishes the final GDS counter at dword `0x79`.

In the fixed scene this changed the sculpt from missing/unstable chunks into a fully covered and
fully filled cube. The user also confirmed that sculpt and tweak-menu jitter was gone. The recorded
view ran at 30 FPS. This is a guest-ordering correction, but the cube is still visually wrong: its
surface is a regular grid of rounded panels.

### The remaining defect is later in the sculpt draw path

The current draw map is:

`ordered records -> VS 0x3706083c cuboid proxies -> FS 0xce3b8413 atlas raymarch/coverage/depth -> fullscreen VS 0xa33ab236 + FS 0xdcc325c2 decoder -> lighting`

The VS uses a fixed 14-index cuboid strip per instance. Captured Param1 atlas IDs are unique, and
all corresponding R8 atlas bricks are nontrivial. The panel silhouette therefore points first to
CE3 sampling/raymarch/discard rather than missing conventional geometry. DCC remains the later
material/color target.

### A3 residency and 63dd handoff were challenged

- A one-run A3 BDA prewarm reported `registered_before=true`; missing first-use residency was
  falsified and the experiment was reverted.
- Captures show equal producer/consumer count and record tables for the `0x63ddac84 -> 0xa3a9e9ef`
  handoff, no focus-probe overflow/comparison mismatch, and matching sampled outputs. This lowers
  the probability of that handoff as the panel source.
- The AMD device exposes native shader float64, so A3 is not using the lossy FP32 fallback.

### Next falsifier

The CE3 trace records both sample sites, exact atlas neighborhoods, and the discard condition for
one manually selected cube draw. A sample mismatch targets image/sampler/synchronization; matching
samples with a wrong discard target wave/EXEC/discard lowering; matching samples and discard move
the investigation to VS370 parameters, association, or LOD.

## Historical August 19 investigation

## Correction to the August 17 conclusion

The August 17 snapshot described full-screen 3D rendering at 30 FPS as confirmed. Later repeated
tests contradicted that conclusion: sculpts, paint/flecks, and characters remained absent; tweak
panels could be black; and some output was compressed, malformed, or unstable. The 897-command
capture remains useful evidence, but it does not establish correct geometry.

The current evidence supports a narrower conclusion: Dreams reaches the indirect geometry path,
but its ordered traversal output is not generated with correct guest semantics.

## Dispatch-base correctness regression

Dreams traversal shader `0xb535c6c8` is submitted in ordered direct batches using
`vkCmdDispatchBase`. The source had lost the compute-pipeline flag required when base workgroup IDs
are nonzero.

The August 19 source now:

- records `uses_ordered_count` during shader-info collection;
- creates those compute pipelines with `VK_PIPELINE_CREATE_DISPATCH_BASE_BIT`; and
- keeps the existing direct traversal `vkCmdDispatchBase` path.

This correction produced the first repeatable visible change tied to one isolated code change:
previously missing geometry became oversized grey shapes. Those shapes jittered when the camera
moved, so the records or positions were still wrong. The correction is retained because the old
pipeline/command combination was invalid and hid the next failure.

## `DS_ORDERED_COUNT` is not an ordinary atomic

Address-unit caution: the field summary below records the August 19 interpretation of later RDNA
documentation. The September 2 investigation reopened whether Liverpool's encoded M0 high-half
must be divided by four when converted to a host GDS dword index. See the current section above;
do not use this historical wording alone to choose counter addresses.

AMD's RDNA instruction-set documentation defines `DS_ORDERED_COUNT` as a wave-ordered operation.
Requests may reach GDS in arbitrary execution order, but are processed in guest wave-creation order.
The operation is issued once per wave.

Important encoded state:

- M0 high 16 bits: ordered-count base in dwords;
- M0 low 16 bits: wave-crawler increment and logical wave ID;
- `offset0`: selects the ordered counter;
- `offset1` bit 0: `wave_release`;
- `offset1` bit 1: `wave_done`;
- `offset1` bits 5:4: counter operation.

Reference: [AMD RDNA 3.5 Instruction Set Architecture](https://www.amd.com/content/dam/amd/en/documents/radeon-tech-docs/instruction-set-architectures/rdna35_instruction_set_architecture.pdf).

The current shadPS4 implementation is incomplete:

- translation broadcasts the source from the first active guest lane;
- resource tracking derives the GDS counter from M0 high bits and `offset0`;
- resource tracking then discards M0 low bits containing guest logical-wave ordering data;
- SPIR-V emission uses a host-subgroup leader and a normal atomic exchange/add/increment;
- decoded `wave_release` and `wave_done` bits do not drive an ordered guest-wave queue.

Serializing host workgroups does not solve this because multiple guest waves inside one workgroup
can still reach the atomics in the wrong order. A global GPU spin loop is also unsafe without a
scheduling proof: later waves can occupy execution resources while an earlier wave has not run,
causing deadlock.

## Ordered-count instructions captured in Dreams

Traversal shader `0xb535c6c8` contains four relevant operations:

| PC | Source | Destination | `offset0` | `offset1` | Meaning |
| --- | --- | --- | --- | --- | --- |
| `0x126c` | `v2` | `v2` | `0x08` | `0x01` | release |
| `0x1294` | `v1` | `v23` | `0x18` | `0x01` | release |
| `0x12b4` | `v1` | `v22` | `0x0c` | `0x01` | release |
| `0x12cc` | `v1` | `v21` | `0x10` | `0x03` | release and done |

This pattern is consistent with several ordered allocations followed by completion of one guest
wave. It makes the ignored logical-wave ID and completion state directly relevant to Dreams rather
than merely theoretical ISA details.

## Geometry draw and producer evidence

The most expensive captured draw was:

- type: indexed indirect;
- draw ordinal: 315 in that capture;
- vertex shader: `0xd25db925`;
- fragment shader: `0x3f6e1a00`;
- maximum command count: 897;
- indirect stride: 20 bytes;
- measured time: approximately 46.327 ms.

This is the sprite/fleck geometry draw. It connects the traversal/compaction output to the missing
sculpt and paint presentation. It is not evidence that the draw should be skipped, capped, or
replaced by conventional triangle rendering.

Earlier traces found two different states:

- an early state where all 897 indirect command slots were zero; and
- a later state with 897 nonzero commands, 504 with active instances.

The later count did not guarantee valid positions or record ordering. The August 19 malformed grey
geometry demonstrates why command nonzero counts alone are insufficient validation.

Other relevant shader IDs:

- traversal: `0xb535c6c8`;
- queue producer: `0x2bfebd3c`;
- scene compaction: `0x3937a849`;
- indirect arguments: `0x90272fc4`;
- sprite/fleck geometry vertex stage: `0xd25db925`.

## What the scene tests established

- Local tutorial and creation logic exists; the black result is not explained solely by unavailable
  online scene downloads.
- The imp moves and reacts, UI navigation works, and gadget logic can execute.
- Gadgets can render and were eventually placeable.
- Sculpt and paint placement can produce audio feedback while leaving no visible/selectable result.
- A thin colored strip or dot changes with scene content, and moving a colored imp through the
  affected corner can tint the whole output briefly. This is consistent with valid pixels being
  generated with corrupt positioning or extent, not with the entire renderer being idle.
- Looking down in creation scenes can expose grey lines or shapes at angle-dependent positions.
- The dispatch-base correction expanded that evidence into visible but oversized grey geometry.

## Save-space accounting

Dreams reported `4.00 GB` local usage against a `1.00 GB` limit even with zero creations shown.
The emulator used host directory byte size divided by the PS4 block size and subtracted it from an
unsigned block count. Sparse/allocation behavior and rounding could make used blocks exceed the
declared maximum, underflowing free space.

The corrected path:

- counts regular files recursively;
- rounds each file to a 32 KiB PS4 save block;
- clamps free blocks to zero rather than allowing unsigned underflow; and
- uses the same calculation for directory search and mounted-save information.

After this correction the tested profile could create another scene. This is a save-API correction,
not a host disk-space workaround, and it does not increase the game's real 1 GiB save limit.

## Startup, offline behavior, and performance

- Startup has reached the real Dreams intro, Continue, consent/EULA, Preferences, tutorial logic,
  DreamShaping, and local creations.
- Earlier builds displayed all three startup pages correctly; later experiments regressed some of
  them to black.
- The intro decoder reaches EOF and produces host frames, but current presentation can be black and
  audio can be severely delayed.
- Offline NP/WebApi/DNS/service behavior is sufficient to reach tested local content. Community
  servers and PSN entitlement behavior are not recreated.
- Broad diagnostics and repeated `scheduler.Finish()` calls can create artificial 1 FPS behavior.
  The earlier stable scene was approximately 15-16 FPS; the malformed-geometry candidate was about
  12 FPS in one captured scene.

## Crash evidence

Selecting a trigger-zone gadget produced a host exception record with code `0xe06d7363`. That is a
Microsoft C++ exception code, but the available record contains no useful guest stack or exception
message. It is preserved as a reproducible symptom, not assigned to the 3D renderer without further
evidence.

## Highest-value next implementation

Preserve M0 low-bit logical-wave data through IR and implement ordered-count release/done behavior
in guest wave order. Validate it against one fixed scene using command contents and visible output,
not only command counts. A candidate passes only if:

1. tweak panels remain visible;
2. geometry does not move when only the camera changes;
3. a sculpt preview and placed sculpt are visible and selectable;
4. paint/fleck strokes render;
5. gadgets and existing UI remain correct; and
6. performance is measured with all forced-wait diagnostics disabled.
