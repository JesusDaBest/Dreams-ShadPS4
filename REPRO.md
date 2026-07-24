# Repro Notes

## Local Setup Used During Investigation
- Windows
- Local source build of `shadPS4`
- Game: `Dreams`
- Serial: `CUSA04301`

## Important Local Paths
- Source tree used for investigation:
  - `<shadps4-source-root>`
- Built executable used during testing:
  - `<shadps4-build-exe>`
- Game boot target:
  - `<dreams-eboot-path>`
- Logs:
  - `<shadps4-log-dir>`
- Automated run harness:
  - `<investigation-root>\run_dreams_clean.ps1`
- Archived automated run logs:
  - `<investigation-root>\runlogs`

## Repro Pattern Seen During This Investigation
1. Launch the local test build with Dreams.
2. Early startup reaches real flip generations and intro AV progress.
3. Dreams-specific synthetic same-buffer flips help push boot farther through the early stages.
4. The intro reaches EOF.
5. The tracked EOF 10-bit guest source is still blank, but AV host frames are available.
6. The active alternate EOF current buffer is also blank during the strongest stable runs.
7. The current best baseline prefers the AV snapshot host frame after EOF and stays alive instead of taking the old late crash.
8. Extending CPU-only bootstrap through the full pre-AV stage produced a fresh Friday, July 24, 2026 full-timeout run.

## Earlier User-Facing Symptoms Seen Across Runs
- Sony logo frozen while intro audio continued
- Black screen with intro audio
- One earlier milestone run showed the Dreams intro video at extremely low FPS before crashing
- A separate frame/debug viewer has at times shown valid intro content even when the main window did not
- Newer stable runs can avoid crashing, but still stall after the intro/EOF handoff
- Friday, July 24, 2026 reconfirm runs show the baseline is still somewhat flaky and can fall back to early device loss
- The freshest stable archive on Friday, July 24, 2026 is:
  - `2026-07-23_192254_2026-07-24_preav_cpu_only_bootstrap_probe`

## Why This Repro Matters
- The problem is no longer just "Dreams shows nothing" or "Dreams immediately crashes"
- The investigation has moved into a narrower post-intro / post-EOF handoff problem
- New debugging should start from the Friday, July 24, 2026 stable-with-diagnostics baseline:
  - `2026-07-23_192254_2026-07-24_preav_cpu_only_bootstrap_probe`
