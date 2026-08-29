# Dreams D25 shape finder

This tool has one purpose: identify which real D25 selector data produces the correct sculpt shape.
It does not render invented boxes, reuse a captured post-VS picture, or change emulator behavior.

`finder.py` replays the exact `Position0` calculation from Dreams vertex shader `0xd25db925` on the
CPU. It consumes the real indirect commands, indices, D25 b0 selector list, packed b1 vertices,
128-byte b2 records, resolved 64-byte transform, and effective SGPR0..SGPR4 values. Every binary is
SHA-256 checked before use. Missing, truncated, out-of-range, and non-finite inputs are rejected.

Live mode sends each exact selector payload to the running emulator, so Dreams itself renders the
shape. **No** atomically publishes the next unique payload; **Yes** saves a self-contained exact
winner bundle. Byte-identical candidates are shown once while all original names and provenance are
retained in the winner.

## Bundle

`bundle.json` schema 1 contains shared real inputs and an ordered list of b0 candidates:

```json
{
  "schema": 1,
  "shader_hash": "0xd25db925",
  "index_type": "uint16",
  "indices_first_index": 0,
  "topology": "triangle_strip",
  "primitive_restart": false,
  "files": {
    "draw": {"path": "draw.bin", "sha256": "..."},
    "indices": {"path": "indices.bin", "sha256": "..."},
    "b1": {"path": "b1-packed-vertices.bin", "sha256": "..."},
    "b2": {"path": "b2-records.bin", "sha256": "..."},
    "transform": {"path": "transform.bin", "sha256": "..."},
    "userdata": {"path": "userdata-effective.bin", "sha256": "..."}
  },
  "candidates": [
    {
      "name": "pre-image-alias-compute-output",
      "selectors": {"path": "candidates/pre.bin", "sha256": "..."},
      "config": {"kind": "raw_u32"},
      "provenance": {"capture_phase": "pre-image-alias-compute-output"}
    },
    {
      "name": "post-image-alias-bound-data",
      "selectors": {"path": "candidates/post.bin", "sha256": "..."},
      "config": {"kind": "raw_u32"},
      "provenance": {"capture_phase": "post-image-alias-bound-data"}
    }
  ]
}
```

`draw.bin` may contain multiple 20-byte `VkDrawIndexedIndirectCommand` records; every active command
and instance is replayed. For one contiguous index capture, `indices_first_index` states which logical
index-buffer element is first in `indices.bin`. Non-contiguous commands use `index-slices.bin` plus a
SHA-256 checked `files.index_slices` TSV with `command`, `byte_offset`, and `index_count` columns.
Sparse b2 captures require `files.b2_record_ids`, a u32 selector ID for every 128-byte captured record;
without it b2 must be contiguous from selector zero. `transform.bin` is the already-resolved 64 bytes
at the D25 transform slot. `userdata-effective.bin` is exactly five little-endian u32 values.

The first candidate is deliberately the exact currently-corrupt post-alias bytes. Its replay alone
models Vulkan robust out-of-bounds b2 reads as a zero-filled 128-byte record, matching the current
corrupt render. Exact fix candidates still reject every missing b2 ID. Its replay must match the shape
seen in Dreams before any candidate can be trusted. When captured, exact
`producer-1e7-b0-post.bin` is candidate 2, followed by the pre-alias D25 candidate. Their names and
capture phases remain distinct.

## Import a capture

No hand-written bundle JSON is needed. A canonical capture directory contains `manifest.tsv`, the
shared binaries, `b0-current-corrupt.bin`, and optionally
`b0-pre-image-alias-compute-output.bin`. Importing copies and hashes everything, validates all replay
inputs, and enforces the current-corrupt candidate as candidate 1:

```powershell
python tools/tests/dreams_d25_shape_finder/import_capture.py `
  D:\capture\raw D:\capture\finder-bundle
```

## Run

Run the real-emulator shape finder with the control-file path watched by shadPS4:

```powershell
python tools/tests/dreams_d25_shape_finder/finder.py D:\capture\bundle.json `
  --live-control D:\capture\d25-live-selectors.control `
  --live-fingerprint-manifest D:\capture\raw\manifest.tsv
```

The live window contains only the current candidate label and **No**/**Yes**. Closing it removes the
control file only when the file is still the exact one written by this process. The finder writes raw
selectors atomically to the stable sibling `d25-live-selectors.control.candidate.bin`, then atomically
publishes the emulator's `schema=1`, `enabled=1`, and `candidate=...` text control. The standalone CPU
replay viewer remains available by omitting `--live-control`.

The optional fingerprint manifest adds only the captured command address, count address, command
stride, maximum count, selector address/size, and transform address. It deliberately omits
`draw_count` and `commands_hash`, which would require per-draw GPU readback. All seven inexpensive
fields are required when this option is used.

```powershell
python tools/tests/dreams_d25_shape_finder/finder.py D:\capture\bundle.json
```

Validate without opening a window:

```powershell
python tools/tests/dreams_d25_shape_finder/finder.py D:\capture\bundle.json --validate
```

The default winner is `winner.bundle` beside the input manifest. Use `--winner` to choose another new
directory. Existing output is never overwritten.

## Strict candidate builder

`build_candidates.py` accepts prebuilt raw selector captures. It can also create only interpretations
authorized by capture metadata: raw u32, float32-to-u32, declared packed-u32 channels, or a canonical
CPU reconstruction of `0x1e7dbccf`. It never generates random shifts, masks, permutations, or geometry.

Exact raw u32 captures are always ordered before reconstructions and conversions. The canonical 1e7
reconstruction is explicitly lower-confidence because GPU atomic order is not recoverable from the
input buffers. It requires all of these captured values and makes no defaults:

- exact pre-1e7 `active-b2-word29.bin`;
- exact pre-902 counter seeds;
- `ReadConst#2`, `ReadConst#12`, `ReadConst#13`, and GDS dword 160.

It uses `active_count = min(ReadConst#12, GDS160)`, replays the shader's bucket/counter equations in
sequential thread-id order, and rejects missing counters or a selector output with unexplained holes.

```powershell
python tools/tests/dreams_d25_shape_finder/build_candidates.py `
  D:\capture\bundle.json D:\capture\selector-sources.json
```

Example source entries:

```json
{
  "schema": 1,
  "sources": [
    {
      "name": "pre-image-alias-compute-output",
      "path": "active-b0-pre-alias.bin",
      "storage": {"kind": "u32"},
      "provenance": {"capture_phase": "pre-image-alias-compute-output"}
    },
    {
      "name": "canonical-1e7-reconstruction",
      "storage": {"kind": "d25_1e7_replay"},
      "active_b2_word29_path": "active-b2-word29.bin",
      "pre902_counts_path": "pre902-counts.bin",
      "read_const_2": 1000,
      "read_const_12": 900,
      "read_const_13": 897,
      "gds_160": 900,
      "provenance": {"capture_phase": "pre-1e7"}
    }
  ]
}
```

## Position replay

For each indexed vertex, D25 unpacks three u16 coordinates from b1 and evaluates, with binary32
rounding after every multiply/add:

```text
local = u16 * float32(2 / 65535) - 1
object.x = local.x*r12 + r21 + local.y*r15 + local.z*r18
object.y = local.x*r13 + r22 + local.y*r16 + local.z*r19
object.z = local.x*r14 + r23 + local.y*r17 + local.z*r20
Position0 = transform_column0*object.x
          + transform_column1*object.y
          + transform_column2*object.z
          + transform_column3
```

The b2 record is selected by `b0[effective_SGPR4 + gl_InstanceIndex]`. The packed vertex is selected by
`b1[effective_SGPR3 + gl_VertexIndex]`. This matches the dumped D25 IR rather than a guessed cube model.

## Tests

```powershell
python -m unittest discover -s tools/tests/dreams_d25_shape_finder -p "test_*.py" -v
```
