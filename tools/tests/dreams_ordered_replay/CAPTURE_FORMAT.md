# Aligned capture contract

One directory must contain every file below from the **same guest chain and frame**. A matching
filename captured at another time is not aligned evidence.

The live producer remains idle until an empty `capture.request` marker exists in this directory.
It removes that marker only when it accepts the first nonempty d8 pass-0 chain.

| File | Required contents |
| --- | --- |
| `dispatch.json` | Schema version, shader (`7aa` or `016`), active count, exact dispatch dimensions, group size, initial/final ordered counters, draw instance count, d8 pass count, index size, vertices per instance, and buffer record counts. |
| `gds-pre.bin` | Raw GDS immediately before the d8 chain. |
| `d8-input.bin` | Active d8 input U32 values in guest index order. |
| `d8-output-selectors.bin` | Active selectors after pass 9, before 7aa. |
| `gds-post-d8.bin` | Raw GDS after pass 9, including dwords 341–350. |
| `7aa-lookup.bin` | Binding-2 U32x2 records covering every captured `selector >> 10`. For 016, the analogous direct U32x2 input. |
| `7aa-output.bin` | Binding-0 U32x2 output after the dispatch, including both seeded output ranges. |
| `gds-post-7aa.bin` | Raw GDS after 7aa/016, including 351/352 or 353/354. |
| `draw-indirect.bin` | The exact 20-byte indexed indirect command consumed by VS370. |
| `indices.bin` | The exact index sequence and width declared in `dispatch.json`. |
| `valid-reference-ids.bin` | Sorted unique downstream low-24-bit reference IDs valid for this draw. |
| `candidate-records.bin` | U32x2 identity for every geometry entry in `candidate-interface.bin`. |
| `candidate-interface.bin` | Post-VS float4 Position/Param0/Param1/Param2 for every candidate record, in the layout declared by `dispatch.json`. |
| `vs370-interface.bin` | Post-VS interface for the actually consumed baseline draw. |
| `vs370-validity.bin` | Per-vertex validity masks for the baseline interface. |

The two candidate-geometry files are necessary for the interactive viewer. A baseline
`vs370-interface.bin` only describes records that the current output happened to draw. It cannot
reconstruct a different candidate that selects another valid lookup record. The viewer must report
`candidate-records.bin` or `candidate-interface.bin` as missing instead of inventing that geometry.

All integer binaries are little-endian. Pair records are tightly packed `<u32 lo, u32 hi>`. The
capture producer must write byte sizes and SHA-256 values into `dispatch.json`; the offline loader
must reject mismatches before scoring candidates.
