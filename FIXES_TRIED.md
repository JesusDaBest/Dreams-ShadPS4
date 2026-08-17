# Fixes Tried

## August 14-17 Main-Menu/Tutorial Branch

### Confirmed progress

- Reached the real Dreams intro, Continue, consent/EULA, and Preferences flow.
- Entered tutorial logic and spawned a controllable imp.
- Added real-time pacing for `/Mm-Dreams-logo.mp4`.
- Reset AAC decoder state before reinitialization.
- Restored scheduled blank-frame presentation for the startup pages.
- Added narrow Dreams runtime guards for the repeated low-FPS Preferences warning at guest offset
  `0x948810` and a scene gate at `0x9c44f7`.
- Added offline NP, WebApi, presence, DNS, socket, and service-resolution compatibility needed to
  move startup forward without live Dreams servers.
- Added/fixed Dreams shader support for dynamic `S_SETPC`, control-flow masks, `mbcnt`, DS 64-bit
  atomics, mixed runtime descriptors, metadata images, and unreachable CFG blocks.
- Implemented `DS_ORDERED_COUNT` translation, counter tracking, scratch state, and a serialized
  direct traversal path.
- Confirmed compute GDS-to-memory release carries live values in the observed path.
- Removed unconditional traversal `scheduler.Finish()` calls from production execution. These waits
  were a proven source of artificial 1 FPS behavior.

### Current incomplete results

- The newest intro test is still invisible and has severely lagged audio, so another stall or AV
  presentation bug remains.
- The tutorial is still black except for a tiny fragment and the imp.
- Queue producer `0x2bfebd3c` receives zero scene records; downstream traversal consequently has no
  work.
- Production indirect traversal uses native dispatch because CPU readback plus one-dispatch-per-
  workgroup ordering is prohibitively slow.

### Ruled out or corrected

- The queue is not known to contain thousands of failed Gaussian splats; the captured queue is
  empty.
- The black tutorial is not explained only by missing online scene downloads. Local tutorial logic
  and imp data are present.
- A generic missing compute `RELEASE_MEM` GDS copy is not supported by the capture; increasing GDS
  values reached memory correctly.
- Tutorial progression save flags can bypass onboarding state, but they do not repair rendering.
- Broad diagnostics are not valid performance tests because they intentionally force GPU waits.

### Most useful next direction

- Trace the writer of the producer record count at `SRT + 0x64` without full GPU stalls.
- Profile intro decode/presentation independently of scene traversal.
- Preserve the known-good three-screen startup scheduling while working on the scene queue.
- Design a GPU-only ordered indirect traversal path after nonzero scene records are restored.

## Real Fixes or High-Value Progress

### 1. Dreams crash-draw interception
- Added Dreams-specific handling for the recurring crash-draw path at `0x300400000`
- This was one of the first changes that turned the problem from an instant wall into something debuggable

### 2. Same-buffer synthetic flips
- Added a Dreams-specific same-buffer synthetic flip path
- This helped Dreams move past early present starvation instead of stopping after the first few real flips

### 3. Better bootstrap behavior
- The bootstrap nudging logic was adjusted so Dreams can more reliably reach later startup stages and `flip_snapshot gen=4`

### 4. Host flip-snapshot presenter override
- Wired the existing Dreams flip-snapshot host-frame path into the real VideoOut flow
- This means the main presentation path can now use the captured Dreams snapshot directly instead of always depending on the fragile guest image path

### 5. CPU-only synthetic bootstrap completion
- Converted the dangerous later bootstrap submit into a CPU-only completion path once the flip snapshot generation is established
- This removed the old crash around the synthetic `flip_arg=4` handoff and allowed automated runs to stay alive much longer

### 6. Post-intro stage identification
- Confirmed the later-stage render target `0x302ef0000`
- This was a major milestone because it proved Dreams was progressing beyond the older intro bottleneck

### 7. Post-EOF `GfxEOP` suppression
- Added Dreams-specific suppression for post-EOF `GfxEOP` timeout wakes
- This matters because reintroducing those wakes repeatedly pushed Dreams back into the old bad late-tail route

### 8. Post-EOF 10-bit tail dispatch disable
- Disabled the special path that was still re-allowing the dangerous post-EOF 10-bit tail dispatch
- This was the first step that produced a clean non-crashing timeout run on Friday, July 24, 2026

### 9. Blank tracked 10-bit EOF detection
- Added a narrow EOF-only check for whether the tracked 10-bit source is actually blank
- This avoided the earlier too-broad probe that disturbed pre-EOF timing

### 10. EOF AV snapshot fallback
- When the tracked 10-bit EOF source is blank, the presenter can now prefer the Dreams AV snapshot host frame
- This proved that the emulator does have usable AV host-frame content even when the tracked guest source stays black

### 11. Disable post-EOF same-buffer keepalive submissions
- The same-buffer monitor used to keep issuing post-EOF synthetic keepalive submissions
- That repeatedly reopened the bad `0x85f67046` -> `0xff751373` -> access-violation path
- Disabling that post-EOF keepalive path produced the current best stable baseline:
  - `2026-07-23_190421_2026-07-24_eof_guest_buffer_probe`

### 12. Status-only acknowledgment for skipped post-EOF submit-and-flip
- Dreams-specific post-EOF submit-and-flip skipping is now acknowledged through VideoOut status bookkeeping instead of the older patched-submit probe
- This preserves the no-crash baseline more reliably than trying to fake more GPU-side progress at that point

### 13. EOF current-buffer blank probe
- Added an EOF-only probe for the active current 10-bit buffer in addition to the tracked source probe
- This confirmed that the main alternate EOF candidate at `0x300400000` is also blank during stable runs
- That matters because it rules out the simpler theory that the presenter was just choosing the wrong already-rendered 10-bit guest surface

### 14. Pre-AV CPU-only bootstrap extension
- The same-buffer bootstrap path used to switch away from CPU-only synthetic completion as soon as tracked source capture appeared
- Extending the CPU-only path through the whole pre-AV bootstrap window produced a new Friday, July 24, 2026 full-timeout archive:
  - `2026-07-23_192254_2026-07-24_preav_cpu_only_bootstrap_probe`
- That run still ended in the same known EOF idle overlay loop, but it avoided the old early flip-8/9 crash fork on that attempt

### 15. EOF AV snapshot fallback refinement
- The presenter fallback was tightened so EOF can prefer AV host frames when the tracked 10-bit source is present but blank
- This is now confirmed in real runs through repeated:
  - `Prepared Dreams AV host frame 1280x720 format=R8G8B8A8Unorm`
  - `eof_av_snapshot`

### 16. Disabled the old `0xff751373` custom shader-patch path
- The old local patch path for `0xff751373` was forcing a minimal compute layout and skipping normal runtime descriptor binds
- That path is now disabled in source so current crashes are observed against the real compiled shader path again

### 17. EOF `GfxEOP` timeout bridge re-enabled through EOF
- The Dreams-specific timeout bridge was allowed to keep fabricating/synthesizing `GfxEOP` progress after EOF instead of suppressing those wakes
- This changed the late state from a pure post-EOF timeout starvation loop back into a real tail-compute execution path
- That was useful because it exposed the current real tail-resource problem instead of only hiding it behind empty waits

### 18. `0xff751373` null-descriptor EOF tail skip
- A narrow skip was added for the exact post-EOF case where `0xff751373` reaches the tail with only dummy buffers and null `1x1` image bindings
- This now works and shows up as a real `skip shader_a=0xff751373 dims=8x8x48` tail event instead of forcing the shader to run on obviously bad descriptors

## High-Value Failed or Incomplete Experiments

### Forced EOF transition
- Forcing Dreams video EOF did move execution into the later `0x302ef0000` stage
- That was useful evidence
- It also caused device loss when used too crudely

### Main presenter redraw/current-buffer refresh attempts
- Several attempts to force main-window redraw/update after recovery reintroduced Vulkan device loss
- These experiments were useful because they showed the issue is not solved by simply refreshing harder

### CPU-only post-intro fallback variants
- CPU-only fallback changes were tested to see whether GPU copy work was the real submit killer
- Those tests did not fully remove the device-loss path

### Narrow draw/dispatch skips around the post-intro batch
- Multiple increasingly tight skip rules were used to binary-search the failing batch
- These reduced uncertainty, but the final crash still survives, which means the remaining bad actor is earlier or broader than one obvious tail draw

### Stable black-bootstrap branch
- The latest branch no longer immediately crashes in the same spot
- It currently stalls in a black-frame bootstrap loop with only three real flip snapshots and then repeated CPU-only synthetic completions
- That is useful progress, but it is not a real rendering fix yet

### EOF guest-flip reroute
- Rerouting real EOF guest flips through the synthetic completion path made Dreams crash earlier
- This was backed out and is now considered a known bad direction

### Post-EOF wake budget / limited synthetic wake experiments
- Reintroducing bounded wakeups after EOF looked tempting, but it repeatedly reopened the same late crash path
- This was backed out in favor of strict suppression

### Capped EOF synthetic submit-done behavior
- Allowing a limited number of EOF synthetic submit-done completions also reopened the unstable route
- This was removed

### Broad blank-tracked-source probe
- The first version of the blank 10-bit tracked-source check ran too early and disturbed pre-EOF timing
- Narrowing it to EOF-only fixed that regression

### Post-EOF same-buffer keepalive submissions
- These were useful for proving Dreams wanted more visible progress after EOF
- In practice they repeatedly fed the bad late-tail route and caused `0xC0000005` crashes
- They are now disabled on the best current baseline

### Late-tail tracked-takeover probe dispatch passthrough
- A narrow Friday, July 24, 2026 probe temporarily re-allowed late tracked-takeover setup dispatches at high flip generations
- This immediately reintroduced the guest-side `0xC0000005` crash without producing a nonblank EOF guest buffer
- It is now considered a known-bad route

### Post-EOF `0x853f753d` setup dispatch passthrough
- A second narrow probe temporarily re-allowed the skipped `0x853f753d` `2x2x12` post-EOF setup dispatch
- This also immediately reintroduced the guest-side `0xC0000005` crash without making the EOF guest buffers nonblank
- It is now considered a known-bad route

### Real `0xff751373` tail execution with dummy descriptors
- Once the old custom shader-patch path was disabled and EOF `GfxEOP` timeout bridging was allowed through EOF, `0xff751373` executed on real pipeline state again
- Diagnostics on Tuesday, August 4, 2026 showed:
  - all three buffers effectively dummy at `addr=0x1`
  - sampled/storage image bindings collapsed to null `1x1` images at `addr=0x0`
- Letting that path execute is now considered a known-bad route

### Skipping only `0xff751373` is not enough by itself
- Even after the null-descriptor skip for `0xff751373`, the same guest-side read fault still occurs
- The last tail then reduces back to:
  - `dispatch-direct shader_a=0x853f753d dims=2x2x12`
  - `skip shader_a=0xff751373 dims=8x8x48`
- That means `0x853f753d` is still part of the remaining blocker

## What Was Learned From The Failed Experiments
- The remaining blocker is real emulator GPU/presentation logic, not user setup confusion
- The `0x302ef0000` stage is important, but it is not the only piece
- Removing the synthetic-flip crash does not automatically restore real visible content
- A blank tracked 10-bit EOF source does not mean Dreams produced nothing; AV host frames can still exist
- A blank tracked 10-bit EOF source plus a blank active current EOF buffer means the missing content is not simply sitting in the obvious guest-present candidates
- Reopening post-EOF guest-visible flip progress too aggressively is one of the easiest ways to reintroduce the crash
- Switching the pre-AV bootstrap from CPU-only to GPU-visible synthetic progress too early may contribute to the old early flake/crash path
- The post-EOF tail is not one single bad shader:
  - `0xff751373` can clearly be wrong because it receives dummy/null descriptors
  - but skipping only `0xff751373` still leaves the same final read fault
- `0x853f753d` is now the next highest-value suspect because it still executes at the final tail with dummy buffers but a real storage image bound

## Current Most Useful Next Direction
- Keep the new no-crash post-EOF baseline intact
- Push the AV-backed EOF handoff into a real gameplay-rendering takeover instead of reintroducing post-EOF synthetic keepalive churn or narrow compute re-allow probes
- Keep the extended pre-AV CPU-only bootstrap change while measuring whether it really reduces the early flip-8/9 crash frequency
- Revisit the guarded `0x300400000` / crash-draw / late-tail cluster only from the stable Friday, July 24, 2026 baseline
