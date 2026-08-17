# Status

## Game

- Title: `Dreams`
- Serial: `CUSA04301`
- Status date: August 17, 2026
- Playability: **not playable**

## User-visible progress

The current source branch has progressed much farther than the July/August post-EOF prototype:

- The Dreams logo/intro path can execute to EOF.
- The black `Press X`, EULA/consent, and Preferences screens have all rendered correctly in at
  least one build.
- Continue and Preferences became interactive.
- The game entered the opening tutorial rather than remaining permanently in the old loading loop.
- The imp spawned, responded to the controller, and could be moved.

The scene is still unusable. The tutorial is predominantly black; only a tiny green fragment was
seen at the top-left before the imp appeared. Pressing Options could make the imp disappear while
the tutorial continued asking for movement toward invisible geometry.

## Latest regression

The newest test still presents the intro at about 1 FPS and audio at about 2 FPS. Game timing itself
appears approximately correct, but the intro image is currently not visible. This means the recent
removal of explicit traversal-side GPU stalls was valid but not sufficient to fix all AV/presenter
stalling.

## Current rendering blocker

The current blocker is upstream of the Dreams traversal shader:

- Queue producer compute shader: `0x2bfebd3c`
- Observed producer record count: `0`
- Count source: flattened SRT entry 41, corresponding to `SRT + 0x64`
- Main queue buffer base in the captured run: `0x2e3ae0000`
- Main queue size: `0x1800000`
- Queue stride: `8`
- Queue and supporting buffers 3/4: all zero in the capture
- Traversal indirect X dimension: `0`
- Traversal GDS input base/total: `0/0`

The traversal shader therefore has no scene records to process. The immediate research target is
the scene/object publication stage that should populate `SRT + 0x64` and the producer input list.

## Ordered traversal state

The source now translates `DS_ORDERED_COUNT`, tracks its GDS counter selection, and contains a
Dreams-specific ordered traversal path. Direct traversal dispatches can be serialized one
workgroup at a time to preserve guest creation order.

For indirect dispatches, normal execution currently uses native `dispatchIndirect`. Reading the
indirect dimensions back to the CPU and splitting every workgroup is restricted to explicit
diagnostics because that path caused severe frame stalls. Exact indirect wave-order emulation is
therefore still incomplete once the game begins submitting nonzero traversal work.

## GDS release result

The suspected compute `RELEASE_MEM` / GDS-copy failure was not reproduced:

- GDS-to-memory snapshots at offset `0xc00` contained `64,0,0,...` and then increasing third values
  including `1866`, `5557`, `11121`, and `16545`.
- The observed `0xd049fb84` stage wrote data that was visible to the following transfer.
- Current shadPS4 code already carries the compute pipe ID, writes normal release data, copies
  `GdsMemStore`, and signals the corresponding compute interrupt.

This does not rule out a narrower synchronization bug, but a generic missing compute release-memory
implementation is no longer the leading theory.

## Startup and compatibility changes retained

- AAC state is reset before reinitialization to avoid stale decoder state.
- The Dreams logo video uses a real-time clock instead of decode throughput.
- Blank startup frames stay on the present scheduler, restoring the startup screens in the known
  good build.
- Dreams-specific runtime guards bypass the repeated low-FPS Preferences warning and a scene gate.
- NP/network/service stubs allow offline startup to proceed farther without real Dreams servers.
- Shader translation includes Dreams control-flow, dynamic `S_SETPC`, mask, `mbcnt`, DS atomic,
  mixed descriptor, metadata-image, and unreachable-block handling.

## Tutorial bypass

A save-only experiment copied known tutorial progression arrays from a known-good save while
preserving the real save metadata and EULA state. This helped test later paths, but it is not an
emulator fix and should not be included in distributable patches.

## Earlier post-EOF work

The older July/August investigation remains useful evidence. It established AV host-frame fallback,
blank 10-bit guest surfaces, synthetic flip behavior, and invalid descriptor state around
`0x853f753d` and `0xff751373`. The current branch has moved beyond that exact failure mode, so those
findings are historical rather than the closest blocker.
