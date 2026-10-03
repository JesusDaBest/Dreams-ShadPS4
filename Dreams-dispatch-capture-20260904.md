# Dreams: measured ordered-count launch — September 4, 2026

The diagnostic build captured the actual guest launch settings for the clean-core startup failure. The shader uses a direct dispatch of **24 groups, each with 64 threads**, and requires a wave-information input that the current compute prologue does not initialize. This is new runtime evidence, not a working DS_ORDERED_COUNT implementation.

## Observed settings

The matching records occur immediately before translation of shader `0xcbac06d2` fails:

| Setting | Observed value |
| --- | --- |
| Launch kind | Direct |
| Workgroup dimensions | 24 × 1 × 1 |
| Start coordinates | 0, 0, 0 |
| Threads per workgroup | 64 × 1 × 1 |
| Partial-thread register fields | 0, 0, 0 |
| User SGPR count | 2 |
| Workgroup-ID inputs | X enabled; Y and Z disabled |
| TG_SIZE / wave-info input | Enabled |
| Raw dispatch initiator | `0x9` |
| Ordered append | Enabled, per wave |
| Out-of-order dispatch flag | Disabled |
| COMPUTE_MAX_WAVE_ID snapshot | 0 |
| Guest shader address | `0x242a20000` in this run |
| Host physical subgroup size | 64 |

This configures 1,536 guest threads and one 64-thread guest wave per workgroup. It does not measure the actual initial EXEC mask or hardware wave scheduling.

The enabled SGPR layout is: user data in `s0–s1`, workgroup X in `s2`, then TG_INFO in `s3`. The shader extracts its ordered token from `s3`. In the current source, `Translator::EmitPrologue` initializes user data and enabled workgroup IDs but does not initialize TG_INFO. Supporting the instruction therefore also requires supplying this input correctly.

The actual shader dump is still 2,308 bytes with SHA-256 `6d024583de0764ebb5eca74772dfca8ff2d460667c39afd844b070cd14f10ce2`, identical to the earlier clean failure and the matching compiled shader in the ZIP.

## What changed and what ran

Added generic, shader-dump-gated launch logging before pipeline creation. It records direct/indirect kind, shader hash/address, user SGPR count, raw launch flags and indirect argument address when applicable. The compilation log labels its dimensions as register values, avoiding the false assumption that they are current GPU-generated indirect dimensions.

Built all 390 official-core translation units with the existing general buffer/swizzle fixes and incomplete ordered-count draft. The dependency cache reused 108 completed units after a source-boundary pause; the resumed build compiled the remaining 282 and linked in 321.1 seconds. This excludes the initial 108-unit phase. Three BelowNormal compiler processes were used. All 390 source-file hashes were checked against the saved build manifest after capture.

Diagnostic executable SHA-256: `a02bc6fb964f21a4658ea2a8657171481f132c7dcc38b57ffb792559816e10fd`. The frozen baseline executable remains unchanged. The user's launcher, installed emulator and original saves were not modified.

One 31.8-second setup-only run stopped before game loading. Its source/filesystem behavior is consistent with the emulator's first-profile save-migration prompt; no native dialog was captured. The subsequent run used the empty profile directories that had been created, reached the target shader, and exited with `0x80000003` within the first five-second sampling interval (harness duration 5.5 seconds). It still reports unsupported DS_ORDERED_COUNT and shader-translation failure. No game memory patch, shader substitution, fake instruction result, save copy or input automation was used. Shader dumping is disabled again afterward.

## Smallest next implementation test

Use the existing research build's production collect → prefix → replay mechanism in a synthetic **24 × 64** GPU fixture. Seed the counter at 17, vary per-wave contributions including zero, and use bounded delays to disturb completion order. Check every returned reservation, the final counter and exactly-once output writes against an explicitly stipulated ordering. Exercise the collector and replay lowering as well as the prefix pass.

The existing B1 route handles one ordered operation: its collector returns after the first operation. The failing startup shader has two. Also, the current prefix shader scans workgroup indices and does not consume the recorded wave tokens. Passing the single-operation fixture would validate a reusable mechanism under its stated model; applying it to this two-operation shader additionally requires input-dependency, alias and side-effect analysis.

The capture does not establish real PS4 token values/order, token-reset history, result-lane behavior, empty-EXEC behavior or initial GDS contents. In particular, `max_wave_id=0` is a register snapshot, not proof that tokens start at zero. Those facts must not be invented to make the game proceed. The research build's earlier Edit Mode visual defects remain a separate problem from this clean-core startup failure.

## Decompiler follow-up

The shader key and shader hashes were absent as searched literals in both executable analysis copies. A reference to `/app0/shaders/%s.shaders` was located in the ZIP executable, and unwind metadata places it inside a function at `0xa45e80` spanning 189,604 bytes. The reference is an interior instruction, not a function entry. The routine exceeds the small decompilation probe's bound, so no whole-routine decompilation was run. These are loader candidates, not proven dispatch callers.

## Evidence

- [Structured launch facts and run details](Dreams-dispatch-capture-20260904.json)
- [Compact evidence bundle](Dreams-dispatch-evidence-20260904.zip)

Workspace reproduction: `work/run-dispatch-capture.ps1 -Seconds 30` uses the isolated diagnostic runtime; `work/analyze-dispatch-capture.py` parses and checks the target capture. Source diagnostics are in `work/upstream-review/src/video_core/renderer_vulkan/vk_rasterizer.cpp` and `vk_pipeline_cache.cpp`. These remain local, unsubmitted changes.
