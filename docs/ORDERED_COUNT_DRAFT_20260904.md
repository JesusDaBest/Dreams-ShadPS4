# Ordered-count work: current state and next experiment

Updated September 4, 2026. Read this before restarting broad repository searches.

## Verified baseline

- Official source base f47e5f5cfeecffa036861fc221e634336516de11 plus generic buffer and quad fixes.
- Frozen tested executable SHA256 F27E15D833B7B3690C4AD4A52DBE111B5DF7C9AC552E366E3E08F7212535D05D. Its copy remains in work/clean-dreams-runtime-20260904. No newly drafted ordered-count execution code is in that binary.
- Two recorded game runs fail translating DS_ORDERED_COUNT in cbac06d2, then exit0x80000003. See outputs/Dreams-clean-core-test-20260904.md and outputs/clean-core-evidence.
- Exact shader dump and static analysis: work/clean-dreams-runtime-20260904/user/shader/dumps/cs_0x00000000cbac06d2_0.bin and work/cbac-ordered-count-analysis.md. Both instruction results are used. Guest EXEC is not narrowed around these sites. Runtime EXEC and dispatch dimensions are still unknown.

## Isolated draft, not an implemented GPU instruction

work/upstream-review now also contains an ordered-count control decoder, a side-effecting IR node, raw TG_SIZE/dispatch flag decoding, runtime metadata fields, and a shader-dump-gated generic compute metadata log. ShaderMetaVersion is4 because runtime metadata layout changed. The backend entry currently rejects execution explicitly; the frontend still reports the guest opcode unsupported. Do not describe this as implemented or working ordered-count emulation.

Descriptor checks (including all raw16-bit controls) and compilation of the changed IR units passed. Four additional integration units compiled successfully: emit_spirv.cpp, emit_spirv_atomic.cpp, vk_pipeline_cache.cpp, vk_pipeline_serialization.cpp. This is not a full draft-core build or runtime validation. Commands: work/ordered-count-test/run.cmd and work/ordered-count-test/check_draft.cmd.

## Highest-value next experiment

Build the draft after the dependency-aware builder is ready, preserving the frozen executable. Capture one bounded startup with generic shader dumping to obtain dimensions, full/partial thread counts, TG_SIZE/TGID enables, dispatch order bits and COMPUTE_MAX_WAVE_ID for the failing compile. Disable dumping afterward. The existing diagnostic emits this metadata only with shader dumping enabled. This run answers a specific missing-runtime-state question; another identical uninstrumented crash does not.

Success criterion: record the exact runtime launch facts, then use them to constrain a synthetic ordered-count test. Do not treat static `(s2 << 6) + v0` as proof of64 active lanes or invent s3's runtime value.

## Unresolved architectural requirements

- Ordered operations are in wavefront-creation order. Workgroup row-major order has not been proven equivalent.
- TG_INFO has first-wave bit31, ordered term bits16:6, wave count bits5:0; it follows enabled TGID SGPRs densely. Correct term generation/base/wrap still needs evidence.
- A write to COMPUTE_MAX_WAVE_ID resets the internal wave-ID counter. Do not reset its base at every dispatch without justification.
- Liverpool result-lane and empty-EXEC behavior must be reconciled with generation-specific documentation and real code. Later RDNA descriptions alone are insufficient.
- Whole-workgroup serialization can deadlock guest intergroup semaphore/wait programs, even with one wave per group. Cross-workgroup GPU spinlocks also need a progress guarantee. Neither is a general solution by default.

Primary references: AMD SI register guide https://docs.amd.com/api/khub/documents/XFXWgxEsHO9Zh0EMGkPACA/content ; AMD Sea Islands ISA https://docs.amd.com/api/khub/documents/YO8RUh~LIQ5Cx3M_Kq9qkg/content ; LLVM AMDGPUUsage https://llvm.org/docs/AMDGPUUsage.html ; original GCN ordered-count discussion https://reviews.llvm.org/D52944 . User may have access to hardware tests later; it is not currently confirmed.

## Efficiency rules for this investigation

1. Keep the baseline, exact inputs, decisions and failed approaches recorded here. Investigate one unresolved question per experiment.
2. Run the smallest relevant instruction/register test first. Build/run Dreams only when it answers a new question or validates a meaningful change.
3. Use compiler-reported per-file dependencies, reuse validated third-party builds, and retain modest low-priority compile concurrency. A changed shared header can legitimately affect many files; never skip dependencies to make a build appear faster.
4. Delegate independent bounded work with explicit file ownership. Avoid duplicate speculative implementations and repeated broad research.
5. Keep one emulator instance, bounded logs and runtime, and exact PID/path checks. Preserve the user's installed launcher and existing saves.


Build workflow is now dependency-aware and its small compiler fixtures pass. First core migration still requires 390 compiles once; it has not run. See outputs/Dreams-development-workflow.md and work/upstream-core-build/DEPENDENCY_CACHE_NOTES.md. The cache helper is reusable for future focused shader-test builds; the older work/upstream-tests/build.py has not yet been migrated.

## User scope clarification

The user explicitly accepts genuine Dreams-only emulator fixes. A fix does not need to benefit other games, and implementing every DS_ORDERED_COUNT mode is not a prerequisite for implementing the verified subset Dreams needs. Preserve correct emulation and the prohibition on hacks/workarounds; do not impose a broader universality requirement on the user's behalf. Prefer behavior justified by actual guest inputs and hardware evidence. Upstream acceptance remains a separate review question.

For physical PS4 tests, prioritize Dreams' two ordered ADD operations: returned values/lane writeback, M0/counter addressing, release/done sequencing with distinguishable wave contributions, and incoming wave-info/token values. Compare identical test inputs and recorded outputs in the emulator. Hardware measurements can settle tested semantics; they do not automatically supply a Vulkan execution design or prove every untested case. Existing host-GPU tests are not measurements of PS4 hardware.