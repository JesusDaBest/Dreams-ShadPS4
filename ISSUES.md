# Open Issues

## 1. Validate the 3D fix across gameplay paths

Full-screen 3D menu content now renders at 30 FPS. Replay the tutorial, sculpt stamping, gadgets,
saved creations, and several premade Dreams to identify any scene classes that still fail or crash.

## 2. Recheck intro video and audio on repeated clean launches

The validated 3D run reached 30 FPS with diagnostic waits disabled. Intro pacing should be retested
separately over several launches to distinguish any remaining AV bug from diagnostic slowdown.

## 3. Indirect ordered traversal is not exact

Direct Dreams traversal can be split into ordered one-workgroup dispatches. Production indirect
dispatch cannot use the same CPU dimension readback without catastrophic stalls, so it currently
falls back to native `dispatchIndirect`. A GPU-only ordered scheme will be needed when traversal
receives nonzero work.

## 4. Reduce the source to upstream-quality fixes

The development snapshot contains extensive gated diagnostics and compatibility experiments. Split
the confirmed workgroup, lane, compaction, and presentation fixes into reviewable changes and audit
their behavior in games other than Dreams.

## 5. Tutorial bypass is only a test aid

Progression flags can skip known tutorial requirements, but they do not create missing scene data.
Do not mistake save editing for a rendering fix or ship a user save as part of the source patch.

## 6. Online functionality is outside the current target

The current target is a usable offline game. Community servers and historical Dreams content are
not expected to work. Offline service stubs must still preserve the state transitions needed to
enter the homespace and DreamShaping interfaces.

## Historical issues to keep in regression coverage

- Zero scene records and zero indirect geometry commands
- Black output or a colored render sliver at the top edge
- Blank tracked and active 10-bit EOF surfaces
- Invalid descriptors around `0x853f753d` and `0xff751373`
- Post-EOF same-buffer keepalives reopening `0xC0000005`
- Early flip/bootstrap device-loss paths
- Repeated low-FPS Preferences warning loop
