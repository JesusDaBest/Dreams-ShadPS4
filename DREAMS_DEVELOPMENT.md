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

## August 22 sculpt-volume A/B

The malformed sculpt path is now localized more narrowly than the August 19 snapshot above:

- Captured sculpt vertex boxes and the four GPU-visible compact-record selectors are complete,
  finite, and coherent. The large `0x800000` selector is an intentional out-of-range sentinel;
  Vulkan robustness correctly returns zero for it. Do not remap it or widen the descriptor.
- The `0x84aa3dc9` sculpt-volume writer performs a leader-only ordered-count atomic, merges the
  leader result with follower zeros, and immediately broadcasts the first lane without an
  explicit reconvergence point.
- Adding a subgroup control barrier between that merge and broadcast improved visible geometry in
  the tutorial and homespace. Edit mode became too slow, so this is evidence for the faulty
  handoff rather than a shippable fix.
- The fragment raymarch shader `0xce3b8413`, R8 volume aliasing, tile-19 addressing, gather write
  coordinates, and captured vertex inputs were audited and did not provide a stronger visual
  fault.

Both observed modes are retained in source. The default is the faster baseline. Set
`SHADPS4_DREAMS_SCULPT_LEADER_BARRIER=1` before shader compilation to enable the visually improved
but slow mode. Changing this option requires deleting the cached
`0x0000000084aa3dc9_0.spv`; otherwise the previous compiled mode remains in use.

The next implementation target is an efficient reconvergence or lane-handoff mechanism that
preserves the barrier mode's visual result without synchronizing thousands of workgroups. Full
guest `DS_ORDERED_COUNT` wave ordering is still independently incomplete.
