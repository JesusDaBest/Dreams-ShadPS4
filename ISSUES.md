# Open Issues

## 1. Post-intro / post-EOF handoff still does not reach gameplay
- Current hard blocker on the Friday, July 24, 2026 baseline
- The best branch can stay alive for the full automated harness window, but reconfirm runs are still flaky
- The game still does not move from the intro/EOF handoff into a known-good playable gameplay scene

## 2. The visible EOF 10-bit guest candidates are still blank
- The current baseline repeatedly records `nonzero=0/256` on the tracked `0x2ffc00000` 10-bit EOF source
- It also records `nonzero=0/256` on the active alternate current buffer `0x300400000`
- That means the obvious guest-present surfaces still are not the real visible content source at EOF

## 3. AV host frames exist, but the handoff after them is still incomplete
- The current baseline does prepare and present Dreams AV host frames after EOF
- That is real progress
- It still does not produce a clean transition into live gameplay rendering

## 4. Some Dreams-specific skip logic may still be too aggressive
- The stable branch avoids the old late crash
- It may also still be suppressing work that is needed for the post-intro takeover into real rendering

## 5. Reopening the old crash route is still easy if the wrong path is reintroduced
- The bad route still comes back if post-EOF same-buffer keepalives, wake budgets, or related synthetic follow-up work are reintroduced
- Narrow late-tail and `0x853f753d` post-EOF compute re-allow probes also reintroduce the bad route immediately
- That makes the current stable baseline important to preserve while debugging the next stage

## 6. Early pre-AV launches are still flaky
- Extending CPU-only bootstrap through the full pre-AV stage helped one Friday, July 24, 2026 harness run reach a clean timeout again
- Some reconfirm launches can still fall back into the older early heap/device-loss route around the flip-8/9 handoff
- That means pre-AV stability improved, but is not yet fully pinned down
