# Dreams ordered-count and visibility WIP — September 2, 2026

This branch preserves the complete experimental source state used for the September 2 Dreams
investigation. It is **not a validated fix or a recommended gameplay build**.

## Runtime status

- The source builds successfully.
- The last executable built from this state had SHA-256
  `CED23677593524511B1392FD77E452CC59C570F888C8A5D3D67A2A008E5A27B8`.
- Entering edit mode with that executable caused severe system-level lag. The exact process was
  stopped and the build must not be presented as the current best checkpoint.
- The log did not show an oversized dispatch before the stall. The cause is not yet proven.

## What this snapshot preserves

- Exact collect/prefix/replay paths and validation for several Dreams `DS_ORDERED_COUNT` shaders.
- Bounded atlas-lineage and GatherVoxels image-write provenance captures.
- GPU buffer/image writer identity and history.
- An f030-to-5ac pre/post pair capture with exact-bound snapshots and invariants.
- Pipeline-cache isolation for capture-instrumented translations.
- CPU read provenance used to test producer-count coherency.

These facilities are primarily opt-in diagnostics. Broad capture modes can force GPU waits and
must not be enabled for performance measurements.

## Unverified address-unit experiment

This snapshot changes generic and title-specific ordered-counter addressing from the earlier
byte-to-dword conversion to an x4 family of indices. Examples include `0x142 -> 0x508`,
`0x100 -> 0x400`, and counter row strides `1 -> 4`.

The correct Liverpool interpretation remains disputed and must be re-established from AMD's
Sea Islands instruction definition and a focused capture. A held-preview run already used
`0x508` for queue producer `0x2bfebd3c` and still published an indirect X dimension of zero, so
the x4 change is not proven to solve the first missing-scene record. Do not merge this experiment
as a correctness fix without a controlled A/B.

## Strongest current evidence

In the decisive empty-scene capture, UI remained visible while the 3D scene disappeared. The
affected indexed draw had `indexCount=14` and `instanceCount=0`. Immediately upstream,
`0x2bfebd3c` published indirect dispatch X = 0, so traversal shader `0xb535c6c8` dispatched
`(0,1,1)` and produced no useful scene work. This localizes that failure upstream of rasterization.

The next low-risk discriminator is the existing producer-count readback at flattened SRT word 41
(`SRT root + 0x64`). If refreshing it changes zero to nonzero, investigate GPU-to-CPU/SRT
coherency. If it remains zero, trace the real writer. If it is nonzero while the producer still
publishes zero, capture the producer's active predicate and ordered-count payload.
