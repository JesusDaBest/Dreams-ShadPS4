# Open Issues

## 1. Scene records are not published

The current highest-priority blocker is the zero record count entering producer shader
`0x2bfebd3c`. The queue and supporting buffers are empty, which leaves traversal indirect work at
zero. Find the compute/CPU stage that should populate the SRT field at `+0x64` and establish whether
the missing update is caused by a skipped dispatch, bad descriptor, unsupported shader operation,
or synchronization error.

## 2. Intro video and audio still present at roughly 1-2 FPS

Removing explicit per-dispatch GPU waits eliminated one proven stall source but did not make the
latest test smooth. The intro is now invisible while its audio is extremely delayed. This must be
profiled independently from scene traversal because the slowdown happens before scene work exists.

## 3. Indirect ordered traversal is not exact

Direct Dreams traversal can be split into ordered one-workgroup dispatches. Production indirect
dispatch cannot use the same CPU dimension readback without catastrophic stalls, so it currently
falls back to native `dispatchIndirect`. A GPU-only ordered scheme will be needed when traversal
receives nonzero work.

## 4. Startup-screen behavior has regressed between builds

All three startup screens were visible in one known-good build, but the newest test no longer shows
the intro. Avoid solving the scene queue by reintroducing blank-frame or AV presentation regressions.

## 5. Tutorial bypass is only a test aid

Progression flags can skip known tutorial requirements, but they do not create missing scene data.
Do not mistake save editing for a rendering fix or ship a user save as part of the source patch.

## 6. Online functionality is outside the current target

The current target is a usable offline game. Community servers and historical Dreams content are
not expected to work. Offline service stubs must still preserve the state transitions needed to
enter the homespace and DreamShaping interfaces.

## Historical issues to keep in regression coverage

- Blank tracked and active 10-bit EOF surfaces
- Invalid descriptors around `0x853f753d` and `0xff751373`
- Post-EOF same-buffer keepalives reopening `0xC0000005`
- Early flip/bootstrap device-loss paths
- Repeated low-FPS Preferences warning loop
