# Dreams: actual official-core test — September 4, 2026

Dreams still fails on this build. Two actual game runs independently reached compute shader compilation and terminated on unsupported `DS_ORDERED_COUNT` instructions in shader `cbac06d2`. Both ended with `structured_control_flow.cpp:802 BuildASL: Assertion Failed! Shader translation has failed` and process exit `0x80000003`. The test harness observed each exit within its first five-second sampling interval; its recorded duration of about 5.4 seconds is not an exact crash timestamp.

This is a concrete blocker for the official-core route. The two earlier general GPU corrections pass their focused tests, but do not provide this missing instruction. No visual improvement, gameplay, or performance improvement is established.

## Build and test identity

- Official emulator source: `f47e5f5cfeecffa036861fc221e634336516de11`, with the general buffer synchronization and constant-index quad-swizzle patches, including cache invalidation.
- All 390 emulator compilation units were compiled from that source tree. No research emulator object files or research host shaders were reused.
- Official Sirit revision `c58f4d441cfdb6905d011a906704716833485486` was verified and rebuilt. The focused official-source GCN executable was relinked with it and passed again: 256 structural selector patterns and five GPU cases with 32 lanes each. This replaces the earlier test's reliance on a reused, modified Sirit library.
- Remaining third-party libraries were reused and hashed. Source/resource checks and a bounded search found no explicit Dreams/title/shader workarounds in the scanned dependency sources. This is not an exhaustive dependency audit or a fully rebuilt dependency supply chain.
- Core executable: `D:\CodexData\Workspaces\2026-09-04\usi\work\upstream-core-build\shadps4.exe`, 67,016,192 bytes, SHA-256 `F27E15D833B7B3690C4AD4A52DBE111B5DF7C9AC552E366E3E08F7212535D05D`.
- Portable runtime: `D:\CodexData\Workspaces\2026-09-04\usi\work\clean-dreams-runtime-20260904`.
- Game executable: `D:\Emulators\ShadPS4\Games\CUSA04301\CUSA04301\eboot.bin`. The runtime reports CUSA04301, App Version 01.00, firmware field `0x9008000`.
- Runtime GPU: AMD Radeon(TM) 8060S Graphics; the log reports physical-device subgroup size 64. The separate focused instruction tests used 32-lane cases.

The build ran with one compiler initially, then three workers at BelowNormal priority after available memory was checked. All compiler processes and test emulator processes have finished. The user's launcher selection, existing saves, and installed emulator were preserved.

## Runtime evidence

The isolated runtime had a pre-existing `user` directory and explicit working directory, with fresh profile/configuration, shader patches disabled, and no copied saves, shader cache, cheats, firmware or title patches. No sibling game-mod overlay was present. Child `SHADPS4_*` environment overrides were removed. Ordinary logging was capped at 16 MiB, with two targeted API categories at Debug. No controls were injected.

`--help` exited successfully with code 0. Its actual output includes `--patch`; the earlier source-only audit's claim that no such option exists was incorrect. No patch option was used.

The first bounded startup (`startup-20260904-134803`) never reached game loading. It created the first empty user home and stopped before writing users.json. This is consistent with the unconditional first-profile save-migration prompt in `UserManager::CreateDefaultUsers`; that interpretation is based on source and filesystem state, not a captured native dialog. The harness stopped only its verified executable/PID at the time limit. The next run used the empty directory created by that first startup and completed profile initialization without moving or copying saves.

The actual game runs were:

| Evidence directory in runtime | Result |
|---|---|
| `startup-20260904-134948` | Unsupported `DS_ORDERED_COUNT`, shader translation assertion, exit `0x80000003` |
| `startup-20260904-135105` | Same terminal failure; built-in shader dumping enabled briefly to capture the exact input |

Shader dumping was disabled again afterward. It produced 41 small files totaling 386,869 bytes. The failing shader is 2,308 bytes and has SHA-256 `6D024583DE0764EBB5ECA74772DFCA8FF2D460667C39AFD844B070CD14F10CE2`. Its local path is `user\shader\dumps\cs_0x00000000cbac06d2_0.bin`. Game shader binaries are not included in the evidence ZIP.

The log also records `sceErrorDialogOpen: called without initialize`, followed by termination of the dialog. Execution continued to the shader failure. This secondary lifecycle observation does not justify auto-initializing dialogs, faking PSN identity, or suppressing errors. Earlier C++ exception notifications also appear in the log before execution continues; the terminal assertion and process exit identify the reproduced stop.

## What the captured shader requires

Decoding the fresh binary against the emulator's GCN opcode/operand tables gives:

| Code offset | Instruction words | Payload | Control |
|---|---|---|---|
| `0x304` | `D8FE0100 0C00000C` | `v12 = popcount(s[18:19])` | ordered ADD, counter offset 0 bytes, release |
| `0x338` | `D8FE0304 0C00000C` | `v12 = popcount(s[2:3])` | ordered ADD, counter offset 4 bytes, release and done |

At `0x2F0`, the shader extracts bits 6..16 of incoming `s3`; at `0x2F8`, it combines that token with `0x0B800000` in M0. It preserves and restores the same token/base for the second instruction. With byte-to-DWORD counter addressing, these are counter indices `0x2E0` and `0x2E1`.

Both returned values are consumed. The first feeds `V_MBCNT_LO/HI` at `0x31C/0x320` to produce `v16`; the second feeds those operations at `0x348/0x34C` to produce `v17`. The shader therefore uses ordered wave reservation bases plus lane prefixes. Zero returns or ordinary atomic arrival order do not reproduce this behavior.

The release/done encoding is supported by LLVM's original GCN implementation/test discussion: [D52944](https://reviews.llvm.org/D52944). AMD's original [Sea Islands ISA, printed page 12-145](https://docs.amd.com/api/khub/documents/YO8RUh~LIQ5Cx3M_Kq9qkg/content#page=243) specifies wavefront-creation order. Later RDNA field descriptions are useful corroboration, but cannot settle every Liverpool-specific lane or empty-EXEC rule.

Not established by this dump: actual dispatch dimensions, launch SGPR configuration proving the identity of `s3`, participating-wave count, complete runtime EXEC state, and all original-hardware lane behavior. Initial indexing suggests groups of 64 lanes, but that is an inference requiring runtime registers.

## Next genuine implementation work

1. Preserve the complete instruction contract in a general decoded descriptor and side-effecting IR: M0/token, counter address, operation, shader type, release and done. Reuse only semantics justified independently of title/shader identity.
2. Implement the incoming compute wave-info SGPR from dispatch/register state for all applicable shaders. Official code omits this input; the research branch reconstructs part of it only for selected shader hashes. A DS handler alone is insufficient.
3. Add production-path conformance cases with synthetic instructions and deliberately reversed wave arrival, two counters, preserved initial values and used return values. Include wave64 versus host subgroup32, conditional/empty EXEC, and release/done sequences once original-hardware semantics are established. Existing short GCN tests bypass parts of production CFG/resource tracking and cannot prove these properties.
4. Choose a GPU execution design that preserves ordered issuance and makes progress. A compiler transformation into count/prefix/replay passes is a possible route only when dependency analysis proves that moving or replaying earlier work preserves all memory and control-flow effects. More general programs require scheduling/continuation machinery. Cross-workgroup spin loops lack a portable forward-progress guarantee. A simpler path needs an explicit proof of a single guest wave and correct lane behavior.

The research branch's ordinary atomic fallback discards ordering information; its shader-selected replay and turnstile paths are unsuitable for direct upstream transplantation. Returning the right final total is insufficient: the tests must check each wave's returned base and resulting writes. No speculative ordered-count emulation was added during this follow-up.

## Reproduction and saved artifacts

The evidence package contains this report, both terminal game logs and run records, the bounded reproduction script, build provenance, dependency/resource checks, and the pinned-Sirit focused test result. It excludes emulator binaries, game data, saves and keys.

`Run-Dreams-startup-test.ps1` reproduces against the saved isolated runtime with a default 60-second limit. It checks for other emulator/launcher processes first and only stops processes whose executable path, PID and creation time match this test. It requires the existing local paths and installed Windows/MSVC runtime components.

There is no stock-versus-patched gameplay comparison or one-patch-at-a-time visual test yet. This run establishes the missing-instruction blocker; it does not establish an effect of the earlier two corrections on Dreams. Changes and evidence remain local; no upstream submission was made.
