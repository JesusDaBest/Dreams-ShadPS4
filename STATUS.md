# Status

## Game
- Title: `Dreams`
- Serial: `CUSA04301`

## Current State
- Not playable yet
- The old immediate post-intro crash path is no longer the default state on the latest baseline
- The best current branch can reach a stable post-EOF idle state instead of dying during the old late-tail crash path
- The new best Friday, July 24, 2026 probe extends CPU-only synthetic bootstrap completion through the full pre-AV stage and reached a full harness timeout again
- That stable state is real, but not yet fully deterministic; some Friday, July 24, 2026 reconfirm runs can still fall back into an early device-loss exit
- The remaining blocker is getting from the intro/EOF handoff into real gameplay rendering, not just stopping a startup crash

## Major Confirmed Progress
- Dreams now reliably reaches early flip generations through a Dreams-specific same-buffer synthetic flip path
- The old synthetic-flip crash can be avoided by switching later bootstrap submits to CPU-only completion
- Post-EOF timeout wake suppression for Dreams `GfxEOP` waits is now in place and helps keep the process alive
- The fatal post-EOF `0x85f67046` / `0xff751373` late-tail path can be blocked by disabling the re-allow path for the 10-bit tail dispatch
- The presenter can now detect a blank tracked 10-bit EOF source and prefer the Dreams AV snapshot host frame instead
- Disabling post-EOF same-buffer keepalive submissions avoids reopening the crash path after the AV fallback becomes active
- The post-EOF submit-and-flip skip is now acknowledged through a lightweight status-only path instead of forcing the old dangerous GPU completion route
- Pre-AV same-buffer bootstrap now stays on CPU-only synthetic completion even after tracked source capture begins

## Best Current Technical Read
- The hard crash is no longer the first blocker on the current baseline
- After the intro reaches EOF, both observed 10-bit guest candidates look blank, but valid Dreams AV host frames do exist
- The current baseline can stay alive by presenting the EOF AV snapshot path while keeping the dangerous post-EOF same-buffer keepalive path disabled
- Extending CPU-only bootstrap through the pre-AV tracked-source phase appears to reduce one of the old flip-8/9 crash forks
- The remaining blocker now looks more like "post-intro gameplay handoff / live guest rendering never taking over" than "presenter chooses the wrong already-rendered buffer"

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
- The current stable loop looks like:
  - pre-AV same-buffer bootstrap can now stay CPU-only through tracked-source capture
  - Dreams reaches intro EOF
  - tracked and active current EOF 10-bit guest buffers stay blank
  - AV host frame preparation succeeds
  - idle EOF fast-path repeatedly prefers the AV snapshot host frame
  - post-EOF waits stay suppressed instead of crashing
  - the game still does not advance into a known-good playable scene

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
