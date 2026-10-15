# Dreams shape rendering correction — October 15, 2026

The supplied build corrected the saved cube, cylinder and vertically stretched sphere in the
user's test scene. The cylinder and sphere are primitives in one sculpt. Other shapes, editing
operations, GPUs and game versions have not been verified by this test.

## Download

**[Download the tested shadps4.exe](https://github.com/JesusDaBest/Dreams-ShadPS4/raw/refs/heads/main/builds/cusa04301-shape-density-fix-20261015/shadps4.exe)**

Use it with your existing Windows shadPS4 launcher and runtime dependencies. The shape-table fix
is enabled automatically. Match the tested settings: **GPU readbacks: Precise**, **pipeline
caching disabled**, and **shader patching disabled**. Leave diagnostic environment flags unset.
Pipeline caching was disabled in the successful test; this build does not add a cache-version
change for the shape correction, so reusing old compiled shaders can bypass the correction.

This download is an emulator executable, not a full portable runtime. The original tested EXE
is preserved byte for byte. Its SHA-256 is `7a91bd313519f0beae84a7e17ea2ee41209baa565d49c19aae1df899aa9551e6`.

This is a branch of the older Dreams source checkpoint. It contains that checkpoint's ordered-count
handling, not nikmparis217-ux's October implementation. It is **not a cumulative successor to the
October 6 tweak-menu build**. That separate [download and its source remain available](TWEAK_MENU_BLUR_FIX_20261006.md).
No new merge of those branches is claimed.

## What changed

Some shape evaluator shaders calculate table pointers relative to their own shader code.
The recompiler had treated those small relative offsets as absolute guest-memory addresses.
The correction recognizes bounded, aligned scalar-table reads and converts them to inline
buffer resources, which are relocated using the actual shader base. Unbounded reads keep the
existing path. This compiler correction is separate from ordered-count implementation changes.

## Evidence and limits

- The original build completed compilation and linking successfully.
- The generated `0x5650b8f7` Vulkan shader passed SPIR-V validation in the original investigation.
- The live log records relocated tables at offset `0x650c` with bounded sizes 172 and 176 bytes.
- On October 15, the user confirmed the saved cube, cylinder and vertically stretched sphere
  rendered correctly using this exact EXE, with normal rendering and diagnostic capture disabled.
- The publication checks verify the EXE hash and source-patch reconstruction. They are not a
  fresh in-game test or a general compatibility certification.

[Validation record](builds/cusa04301-shape-density-fix-20261015/validation.json) · [EXE checksum](builds/cusa04301-shape-density-fix-20261015/SHA256SUMS.txt) ·
[Publication checks](builds/cusa04301-shape-density-fix-20261015/publication-check.json)

## Source

- [Focused compiler correction](patches/dreams-shape-density-inline-tables-20261015.patch)
- [Matching project-source delta](patches/dreams-source-delta-20261015-shape-density.patch)
- [Project-source hash manifest](builds/cusa04301-shape-density-fix-20261015/source-manifest.json)
- [Sources for the embedded optional experiment](builds/cusa04301-shape-density-fix-20261015/source-support)

The focused patch changes only `resource_tracking_pass.cpp`. The matching source delta also
contains the pre-existing fixes and diagnostic code compiled into the tested executable.
Its optional custom volume renderer is off by default and was not used for this test.
Do not apply the focused patch on top of the matching delta; it is already included there.

The matching delta applies to public source commit `b84847d1b6ca14d349bf9e11e91d14ae182e008d` on
`dreams-dev-20260829-full-covered-filled`. Its tree is identical to the private base commit
`ee046166afab8ca04759d5d34b63025a48fae2ae`. The manifest verifies project-owned files separately from dependency trees.

In a fresh source checkout:

```powershell
git clone --branch dreams-dev-20260829-full-covered-filled https://github.com/JesusDaBest/Dreams-ShadPS4.git shadps4-shape-source
Set-Location shadps4-shape-source
git checkout --detach b84847d1b6ca14d349bf9e11e91d14ae182e008d
git submodule update --init --recursive
git apply --check D:/path/to/dreams-source-delta-20261015-shape-density.patch
git apply D:/path/to/dreams-source-delta-20261015-shape-density.patch
cmake -S . -B build-shapes -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-shapes --parallel 1
```

Use the upstream Windows compiler/dependency prerequisites. The tested private build used
Clang-CL, Ninja and Release configuration, with local Abseil and fmt source overrides.
The patch reconstruction has been checked; a fresh dependency download and full rebuild were
not repeated for publication. Independent toolchains may produce different EXE hashes.
The emulator retains its GPL-2.0-or-later licensing; see [LICENSE](builds/cusa04301-shape-density-fix-20261015/LICENSE).
Game data, saves, private configuration and raw game shader captures are excluded.
