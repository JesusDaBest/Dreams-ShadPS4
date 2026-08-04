# Status

## Game
- Title: `Dreams`
- Serial: `CUSA04301`

## Current State
- Not playable yet
- The old Sony-logo-only stall is no longer the main failure mode
- Audio works and Dreams AV host frames are being prepared and presented after EOF
- The remaining blocker is now the post-EOF compute handoff, not the old startup crash

## Latest Confirmed State
- Fresh scripted runs on Tuesday, August 4, 2026 still exit with the same guest-side read fault:
  - `Guest instruction: mov ecx, [rax+0x08]`
  - `Guest access violation: kind=read target=0xffffffffffffffff`
- The latest focused archive for repo notes is:
  - `2026-08-04_050053_2026-08-04_repo_update_run`
- In that run:
  - `0x853f753d` reached the post-EOF tail with dummy buffers (`addr=0x1`) but a real storage image bound at `0x2ff73a000`
  - `0xff751373` reached the same tail with dummy buffers (`addr=0x1`) and dummy `1x1` null-image bindings at `addr=0x0`
  - The last fatal tail still lands in the same area:
    - `dispatch-direct shader_a=0x853f753d dims=2x2x12`
    - then either `dispatch-direct` or `skip` for `0xff751373 dims=8x8x48`
    - then the guest read fault above

## Major Confirmed Progress
- Dreams now reliably reaches early flip generations through a Dreams-specific same-buffer synthetic flip path
- The old synthetic-flip crash can be avoided by switching later bootstrap submits to CPU-only completion
- Post-EOF timeout wake suppression for Dreams `GfxEOP` waits is now in place and helps keep the process alive
- The fatal post-EOF `0x85f67046` / `0xff751373` late-tail path can be blocked by disabling the re-allow path for the 10-bit tail dispatch
- The presenter can now detect a blank tracked 10-bit EOF source and prefer the Dreams AV snapshot host frame instead
- Disabling post-EOF same-buffer keepalive submissions avoids reopening the crash path after the AV fallback becomes active
- The post-EOF submit-and-flip skip is now acknowledged through a lightweight status-only path instead of forcing the old dangerous GPU completion route
- Pre-AV same-buffer bootstrap now stays on CPU-only synthetic completion even after tracked source capture begins
- The old custom shader-patch path for `0xff751373` was disabled, so the current tail analysis is based on the real compiled shader path again
- Allowing the Dreams `GfxEOP` timeout bridge to keep operating after EOF changed the tail from a pure wait-starvation pattern back into a real compute-tail execution path
- A targeted null-descriptor skip for `0xff751373` now works when that shader reaches EOF with only dummy buffers and null `1x1` images

## Best Current Technical Read
- After the intro reaches EOF, both observed 10-bit guest candidates still look blank, but valid Dreams AV host frames do exist
- The current failure is no longer best explained by "wrong present buffer chosen"
- The stronger current theory is that the post-EOF compute tail is running with partially invalid descriptor state:
  - `0xff751373` can bind only dummy/null descriptors
  - `0x853f753d` still has dummy buffers even when its output image is real
- The most likely remaining blocker is now the post-EOF compute/resource handoff into live rendering

## Strongest Current Findings
- The synthetic flip that used to crash can be converted into a CPU-only completion path without killing the process
- The tracked EOF 10-bit source at `0x2ffc00000` is repeatedly blank (`nonzero=0/256`) when Dreams has already produced AV host frames
- The active alternate EOF 10-bit current buffer at `0x300400000` is also blank (`nonzero=0/256`) during the same stable runs
- The AV snapshot presenter path is now active in the real VideoOut flow after EOF, not just in a side viewer
- Reintroducing post-EOF synthetic keepalive behavior repeatedly reopens the bad `0x85f67046` -> `0xff751373` -> access-violation route
- The strongest stable-with-diagnostics archive so far is now:
  - `2026-07-23_192254_2026-07-24_preav_cpu_only_bootstrap_probe`
- That archive reached a full harness timeout with:
  - CPU-only bootstrap still active after tracked-source capture
  - stable EOF AV overlay refresh on `bufferIndex=1` / `0x300400000`
  - tracked EOF buffer `0x2ffc00000` still blank
  - current EOF buffer `0x300400000` still blank
- A Friday, July 24, 2026 reconfirm on the same status-ack baseline still showed flakiness and exited early with device loss:
  - `2026-07-23_191516_2026-07-24_reconfirm_after_853f_revert`

## What Was Ruled Out
- Not blocked on source access
- Not only a "Sony logo hang" anymore
- Not only a synthetic flip crash anymore
- Not only a presenter-main-window wiring issue, because AV host-frame presentation now reaches the real VideoOut path
- Not solved by simply letting the post-EOF same-buffer path keep submitting more flips
- Not helped by reintroducing bounded post-EOF wake budgets or synthetic submit-done behavior
- Not solved by simply exposing another EOF 10-bit guest buffer, because the tracked and active current-buffer probes are both blank

## Closest Current Bottleneck
- The intro/EOF handoff is more stable, but live gameplay rendering still is not taking over
- The current focused bottleneck is:
  - `0x853f753d` post-EOF setup dispatch
  - followed by `0xff751373` post-EOF tail dispatch/skip behavior
  - followed by the same guest-side read fault
- `0xff751373` is now clearly one bad descriptor-state case
- `0x853f753d` is the next highest-value target because it still reaches the crash with a real output image bound

## Current Practical Rating
- Playable state: still low, but better than the old immediate-crash state
- Understanding of the startup/render blocker: moderate-to-good and improving

## Base Source Context
- Investigation tree branch: `dreams-4247-test`
- Source remote base observed during this session:
  - `https://github.com/shadps4-emu/shadPS4.git`
- Current local source/build layout used on Friday, July 24, 2026:
  - source: `<shadps4-source-root>`
  - build: `<shadps4-build-exe>`
