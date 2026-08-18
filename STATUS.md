# Status

## Game

- Title: `Dreams`
- Serial: `CUSA04301`
- Status date: August 17, 2026
- Playability: **not fully verified**

## User-visible result

The current development build reaches offline Dreams content and now renders full-screen 3D. A
clean validation run showed the `Mm Characters` collection with characters, backgrounds, flecks,
UI, and the imp rendered correctly at the game's 30 FPS cap.

This resolves the previous black-screen/top-edge failure in the tested path. It is not yet evidence
that every tutorial, sculpting, creation, and gameplay scene is fully playable. Online community
features and historical Dreams servers remain outside the current target.

## Confirmed rendering repair

The final blocked stage was compute shader `0x90272fc4`, which builds Dreams' indirect geometry draw
commands. Before the fix, all 897 command slots were zero. After the fix:

- 897 of 897 scanned commands were nonzero;
- 504 commands had active instances;
- representative indexed draws had plausible index counts, offsets, and instance ranges;
- the resulting 3D content covered the complete presentation area rather than a top-left sliver.

The correction is source-level and not tied to one saved scene:

- `COMPUTE_PGM_RSRC2.TG_SIZE_EN` is decoded and the requested workgroup-info SGPR is initialized;
- the Dreams indirect-argument shader reads scalar lane masks from their canonical SGPR dwords;
- sprite compaction allocates one contiguous output range per active subgroup;
- `V_WRITELANE_B32` writes only the selected lane;
- invalid mask-shadow restoration was removed from numeric `V_READLANE_B32` and `V_WRITELANE_B32`.

## Scene data result

The earlier capture with `records=0` was not the final state. Later tracing confirmed a live saved
scene record reaching the GPU:

- queue producer `0x2bfebd3c` received `records=1`;
- the first sprite-occlusion pass consumed the exact staged record;
- downstream sprite consumers generated geometry records;
- the indirect-argument stage was the remaining point that reduced those records to zero draws.

Initializing the requested hardware SGPR removed the last undefined scalar value from that shader
and restored its draw-command output.

## Performance

The clean build sustained 30 FPS in the confirmed menu scene. Broad diagnostics such as
`SHADPS4_DREAMS_DIAGNOSTICS`, compute tracing, ordered-counter snapshots, and visibility tracing
perform GPU waits and can reduce the same build to about 1 FPS. They must remain disabled for normal
performance tests.

## Startup and compatibility changes retained

- AAC state is reset before reinitialization to avoid stale decoder state.
- The Dreams logo video uses a real-time clock instead of decode throughput.
- Blank startup frames stay on the present scheduler.
- Narrow runtime guards bypass the repeated low-FPS Preferences warning and the known scene gate.
- Offline NP, network, presence, and service behavior allows startup without live Dreams servers.
- Dreams shader translation includes dynamic control flow, masks, lane operations, ordered counts,
  mixed descriptors, metadata images, and required image/buffer compatibility handling.

## Remaining validation

- Replay the opening tutorial from a clean path and verify all guidance geometry.
- Test sculpt stamping, gadgets, saved scenes, and several premade Dreams for visual correctness and
  crashes.
- Recheck intro audio/video pacing over repeated clean launches.
- Separate essential compatibility code from diagnostics before proposing upstream changes.
- Keep this custom build isolated from other games until emulator-wide changes are audited.

## Safety

The source repository contains no game files, firmware, keys, saves, or PSN credentials. A verified
pre-recovery save backup was retained locally during investigation, but saves are intentionally not
distributed.
