# Imp trails and tweak-menu smush correction — October 4, 2026

## Problem and correction

Moving the imp left persistent copies in menus, including the pause menu. In edit mode, moving the camera past a tweak menu produced a camera-relative smush that obscured the floor grid and gadget/trigger-zone gizmos.

Dreams compute shader `0x85f67046` clears image-backed allocations through a raw buffer. The menu trace recorded its output at `0x2e2890000` (16,711,680 bytes), reused as a sprite render target and later sampled as an image. The raw write did not enter the image-cache invalidation path, which previously handled formatted writes and a separate scene-stream shader.

[The focused patch](patches/dreams-raw-image-clear-cache-refresh-20261004.patch) includes written output buffer 0 of this Dreams compute shader in the existing `InvalidateMemoryFromGPU` path. Matching cached images become GPU-dirty and refresh from the GPU buffer. It does not substitute geometry or disable depth/stencil tests.

## Validation and limits

The Windows build compiled successfully. The user tested the candidate and confirmed that imp copies disappeared from all menus and the tweak-menu smush disappeared too. The user confirmed updating remained correct and authorized installing that build. The installed file matched the tested executable byte-for-byte.

These are user-observed runtime results on the retained research checkpoint, CUSA04301 content VERSION 02.64 / APP_VER 01.00, with Precise readbacks. They are not an automated image regression or a compatibility guarantee for other titles, GPUs or game versions.

Sculpt rendering remains incorrect. Distance-dependent missing tweak-menu content remains unresolved. Sculpt-mode lag and sculpt-limit warnings are also unresolved. The raw-clear fix does not claim to fix those issues.

## Download and source

The [tested executable](builds/cusa04301-imp-smush-cache-fix-20261004/shadps4.exe) has SHA-256:

`E7BAD86D62581BC2D0493D4B880346DE0BCCC31D8AD0DC43EA89484F0DF7599D`

This executable retains prior save corrections and dormant diagnostic facilities. Diagnostic environment flags should be unset for ordinary use. Its title-bar build label still identifies the older research branch and is not a new upstream release number.

Apply the focused patch after the retained August 29 rendering patch; see [SOURCE_BUILD.md](SOURCE_BUILD.md). Source builds can differ in binary hash from this archived diagnostic checkpoint. Experimental standalone replay edits and the unfinished material/lighting capture build are not included in this publication. The older August 29 archive remains available.
