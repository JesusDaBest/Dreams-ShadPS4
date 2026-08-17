# Detailed Discoveries

## Scene publication is empty before traversal

The strongest current capture follows the compute chain into Dreams' scene queue producer:

- Producer shader `0x2bfebd3c` repeatedly reports `records=0`.
- Its flattened user-data buffer has 82 dwords.
- Relevant values include `f18=0x2`, `f22=0x5fff`, and
  `f36-45=0xcab83900,0x2,0xcab82f00,0x2,0,0,0,0,1,0`.
- Producer SRT addresses alternated around `0x243202...` and `0x245202...`.
- The record count is flattened entry 41, matching the field at `SRT + 0x64`.
- The queue at `0x2e3ae0000` was all zero. Supporting producer buffers 3 and 4 were also all zero.
- Buffer 2 was GPU-modified, so it could not be safely interpreted using the same CPU scan.

The following traversal dispatch has an indirect X dimension of zero and GDS input total/base zero.
The black tutorial is therefore currently best explained by missing scene/object publication, not
by thousands of Gaussian splats failing to rasterize.

## `DS_ORDERED_COUNT` semantics matter

Dreams uses `DS_ORDERED_COUNT` in shader `0xb535c6c8`. The operation is not equivalent to a plain
atomic increment. It uses one of multiple ordered counters and returns offsets in guest wavefront
creation order.

The current branch adds:

- instruction translation and IR support;
- correct first-active-lane source handling;
- M0/GDS index tracking;
- enlarged private GDS scratch for ordered entries;
- a serialized direct-dispatch path for the Dreams traversal shader;
- completion handling used by the Dreams-specific path.

Upstream PR `#2899` was also reviewed during this investigation. It identified Dreams as the game
requiring the opcode, but its simple shared-atomic implementation was closed because it did not
model real ordering semantics.

## GDS transfer evidence

An earlier interpretation that GDS snapshots were all zero was incorrect. Exact capture lines show
successful data propagation at GDS byte offset `0xc00`:

- Snapshot 21: `64,0,0,...`
- Target `0xd049fb84` dispatch: third value reached `1866`
- Snapshot 22: third value `5557`
- Snapshot 23: third value `11121`
- Snapshot 24: third value `16545`

This confirms that compute shader writes can become visible to `GdsMemStore` transfer in the tested
path. Old Dreams PR `#798` changes for compute release-memory, pipe-aware notifications, and queue
setup are represented in modern shadPS4 code.

## The 1 FPS diagnostic trap

The Dreams traversal workaround used to call `scheduler.Finish()` before every traversal dispatch,
including roughly ten empty indirect traversal submissions per frame. That serialized the GPU and
produced an artificial 1 FPS failure.

Normal execution no longer performs those CPU readbacks or full GPU waits. Expensive traversal
dimension reads and ordered-counter snapshots are now limited to explicit diagnostic modes.

The latest user test is nevertheless still extremely slow and has invisible intro video, which
proves at least one additional AV/presenter or synchronization bottleneck remains.

## Intro and startup behavior

- Decode reaches EOF and produces frames.
- The Dreams logo video is paced by wall time.
- A catch-up presentation loop made the intro effectively invisible and was removed.
- The startup `Press X`, consent/EULA, and Preferences pages rendered correctly together in a known
  good build.
- Later changes regressed the intro to invisible video with severely lagged audio. Preserve the
  known-good startup scheduling behavior while isolating the remaining slowdown.

## Offline and service behavior

Dreams attempts presence, NP WebApi, DNS, and service resolution during startup. The branch includes
offline-safe responses and selected failure behavior so missing live services do not trap startup.
This is enough for investigation without the original Dreams online servers, but it does not
recreate community content or PSN entitlements.

## Earlier post-EOF descriptor findings

The previous branch found that:

- `0xff751373` could execute with buffers at `addr=0x1` and null `1x1` images at `addr=0x0`;
- `0x853f753d` could have dummy buffers while still binding a real `R32G32Uint` `8x8x48` storage
  image;
- both obvious 10-bit EOF guest surfaces were blank;
- AV host frames were valid even when those guest surfaces were blank;
- post-EOF synthetic keepalive submissions repeatedly reopened a guest read fault.

Those results should remain available for regression comparison, but they are no longer the most
immediate blocker on the current main-menu/tutorial branch.
