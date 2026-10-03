# Dreams game-files inspection — September 4, 2026

No identifiable original Dreams engine source tree or build project was found in the inspected material. The archive is a packaged game image containing a compiled executable, a compiled shader bundle, libraries, configuration, and assets. This conclusion is based on archive inventories and selected content/header inspection; opaque asset blobs were not exhaustively decoded.

## What was checked

- Archive: `D:\Downloads\DREAMS PS4 GAME FILES.zip`, 13,673,103,195 bytes (13.67 GB).
- Indexed all 1,608 regular outer files and all nine nested ZIPs, containing 97,478 further files.
- No conventional C/C++ source files, Visual Studio solution/project files, or PDB files were found in those inventories.
- `TrenTemplate.proto` is serialized binary playlist/template data, not a readable Protocol Buffers schema or engine source file.
- Nested `dev_levels` archives contain hash-named data and metadata. Their names do not establish that they contain a developer source checkout.
- Read selected executable/shader headers and metadata. No code from the archive was executed.

The nested ZIPs were streamed with only a bounded tail retained for their central directories, avoiding extraction of the multi-gigabyte content archives. Outer CRCs for those nine ZIP members were checked by fully reading them. This was not an exhaustive integrity check of every game asset or of every nested member's body.

## The archive differs from the tested installation

| Metadata | This ZIP | Existing game installation |
|---|---|---|
| TITLE_ID | `CUSA08010` | `CUSA04301` |
| APP_VER | `01.00` | `01.00` |
| VERSION | `02.65` | `02.64` |
| SYSTEM_VER | `0x12008000` | `0x09008000` |

These are the exact SFO fields. APP_VER and VERSION must not be conflated: earlier runtime notes correctly recorded APP_VER 01.00, but that alone did not describe the separate VERSION field. The ZIP's changelog also ends at 02.65. Its executable SHA-256 differs from the installed executable.

The [official v2.65 notes](https://docs.indreams.me/en-US/whats-happening/updates/release-notes/dreams/v265), dated February 26, 2025, describe a small backend software maintenance update. They do not identify an emulator-relevant rendering correction. The description does not prove that rendering binaries are unchanged.

## Shader and executable findings

`dd.shaders` is a binary shader pack. Its 604-entry table has valid payload bounds; all payloads contain PS4 OrbShdr metadata and valid code locators under the existing shadPS4 parser. Readable parameter/semantic names are useful reflection information, not a recovered shader-source project.

Both builds have 604 matching pack entry keys, while embedded shader hashes differ. Comparing actual instruction extents from BinaryInfo.length, 546 of 604 corresponding shader code spans are byte-identical and 58 differ. This excludes trailing metadata from the instruction comparison. Byte differences in the other 58 shaders have not been assigned a behavioral cause.

The captured startup shader is byte-identical across these builds: installed hash `cbac06d2` maps by pack key to archive hash `fff5a423`, with all 2,308 instruction bytes matching. Both ordered-count sites at offsets 0x304 and 0x338 retain the same instruction words. B1 shader `4ebeffd2` maps to `c1a85bbd`, with all 364 instruction bytes matching; `84aa3dc9` maps to `e0887e2d`, with all 1,048 instruction bytes matching. Thus different embedded hashes alone do not establish changed instructions, and this ZIP does not supply a different implementation of these three shaders. This does not prove identical runtime inputs or whole-game behavior across versions.

`eboot.bin` is a SELF container with a visible x86-64 ELF header. The visible ELF header declares zero section headers, so it provides no ordinary section-table-indexed debug sections. This limited inspection does not establish that every possible symbol or compressed debug record is absent.

| Selected file | SHA-256 |
|---|---|
| `eboot.bin` | `0a3bf1c7212ae8f341755726870ec5c6ea5404c32cdc7acdd5e5e5ed154d0a43` |
| `dd.shaders` | `02e3d197037a484d60751eb6c18e581a908386a0a1ad3df3823c7d5583a98b33` |
| `param.sfo` | `ebf2fee94a55f81ab93c9b30c9a723e148ef6e5b2ba7ffe7702f32668c4d371f` |

These files may help compare game builds and compiled GPU programs. They do not provide the original engine source needed for a straightforward source-based PC port. No gameplay improvement was tested or inferred.

## Public source-code search

The bounded search found no verified public release of the original Dreams game/engine source and no verified buildable decompilation. It cannot establish whether private or unindexed copies exist.

- [Dreamiverse Transformer](https://github.com/x1nixmzeng/dreamiverse-transformer) is preliminary research into content formats and services; its visible repository contains a README and `.gitignore`.
- [dreams-api](https://github.com/jaames/dreams-api) provides community API/network tooling, not the game engine.
- [Media Molecule's rendering presentation](https://www.mediamolecule.com/blog/article/alex_at_umbra_ignite_2015_learning_from_failure_video) provides technical background and slides, not a complete source release.

The report and companion JSON contain inspection findings only. The installed game, selected emulator, saves, and original ZIP were not modified.
