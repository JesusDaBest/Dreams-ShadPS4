# Dreams Development Snapshot

This branch is the complete experimental source used to investigate `Dreams` (`CUSA04301`) through
August 19, 2026. It is intended for emulator development, not normal gameplay.

## Exact status

- Dreams is **not playable** in this snapshot.
- Startup can reach offline menus, tutorial logic, DreamShaping, and creation scenes.
- UI, the imp, the grid, and gadgets can render; placed gadget logic has worked in testing.
- Sculpt and paint/fleck geometry is still missing or malformed. Characters are not confirmed.
- Tweak-menu surfaces can be black, generated 3D positions can jitter with camera movement, and the
  intro can be black with severely delayed audio.
- The earlier stable executable ran the tested creation scene at roughly 15-16 FPS but left
  sculpts and paint invisible.
- The newest August 19 candidate restores a required Vulkan dispatch-base pipeline flag. It makes
  previously missing geometry appear as oversized grey, unstable shapes. This is a useful
  localization result, not a rendering fix.

Do not describe the August 17 full-screen/30 FPS observation as a verified general fix. Later
same-machine tests contradicted it.

## Confirmed corrections retained

- Startup and offline compatibility changes allow the game to pass the old Sony-logo/service
  blockers without live Dreams servers.
- AAC state reset and wall-clock video pacing address concrete decoder/pacing faults, although the
  complete intro path still regresses.
- Save-data free space now counts rounded 32 KiB save blocks and avoids unsigned underflow. This
  removed the false `4 GB used / 1 GB limit` state in the tested save.
- Shader support added during the investigation includes dynamic control flow, mask/lane
  operations, 64-bit GDS atomics, mixed descriptors, metadata images, and `DS_ORDERED_COUNT`
  translation.
- Compute pipelines that use ordered count are created with
  `VK_PIPELINE_CREATE_DISPATCH_BASE_BIT` before `vkCmdDispatchBase` is used. The previous
  combination was invalid Vulkan usage.

## Main unresolved rendering error

Dreams traversal compute shader `0xb535c6c8` uses `DS_ORDERED_COUNT`. The current backend reduces it
to ordinary atomics and does not reproduce guest wave-creation ordering:

- M0 high bits select the GDS ordered-count base and are tracked.
- M0 low bits containing the logical wave ID/wave-crawler increment are currently discarded.
- `wave_release` and `wave_done` are decoded but do not control a guest-order queue.
- Splitting direct traversal into ordered host workgroup dispatches does not order multiple guest
  waves inside a workgroup.
- Indirect traversal still uses native indirect dispatch unless an expensive diagnostic path is
  enabled.

The captured shader has four ordered-count operations. All release the wave; the final operation
also marks it done. Correct guest-wave ordering is the highest-value next implementation target.

The dominant captured geometry draw was indexed-indirect vertex shader `0xd25db925`, fragment
shader `0x3f6e1a00`, `max_count=897`, stride 20, and approximately 46.3 ms in that diagnostic run.
This connects the malformed traversal output to the missing sculpt/fleck draw rather than proving
that the draw itself should be skipped or capped.

## Protect other games and user data

This snapshot contains emulator-wide changes and extensive diagnostics. Use it only in a dedicated
Dreams build. A local `user` folder can isolate settings, saves, trophies, logs, and caches from a
normal shadPS4 installation. Do not distribute game files, firmware, keys, user configuration, or
saves.

The diagnostics are disabled unless their environment variables or trigger files are supplied.
Many force GPU waits and can reduce performance to about 1 FPS, so diagnostic results are not valid
performance measurements.

Current status, evidence, and priorities are maintained on the repository's `main` branch:

https://github.com/JesusDaBest/Dreams-ShadPS4
