# Random-byte correction and save-ID test — October 3, 2026

## Problem and correction

The checkpoint's `sceRandomGetRandomNumber` generates bytes using `std::rand()`. In the Windows compiler runtime tested here, separate fresh processes return the same default stream, and eight newly created threads independently return that same stream. Random device creation elsewhere can also reseed the host rand stream. Dreams imports this API. The previously preserved metadata contained different creations sharing version ID `vd66ce184be2329`.

[host-entropy-random-20261003.patch](patches/host-entropy-random-20261003.patch) replaces this API's rand stream with `std::random_device` and a byte distribution. Calls are serialized, output is staged before copying to the guest, invalid nonzero null requests are rejected, and provider exceptions return the API's fatal error. This does not change every random device implementation in the emulator. Entropy-provider behavior was tested on the Windows build only; no cross-platform entropy guarantee is claimed.

## Validation

The regression compiles the actual `random.cpp` implementation; only logging and symbol-registration dependencies are stubbed. The old function repeated between processes and produced 128 distinct samples from 1,024 calls over eight threads. The corrected function produced 1,024 distinct samples, differed after host srand resets, and passed 20 separate-process comparisons. Sizes 1 through 64 preserve buffer guards; zero-length and invalid size/null checks pass. No collision in these finite tests is an absolute uniqueness guarantee.

The emulator rebuilt successfully with one compile job. The candidate retained the checkpoint rendering source and the separate October 2 save-mount delta. In an isolated portable profile, the user saved one-, two- and three-cube scenes, reopened them with correct contents and previews, and checked the first two after a full emulator restart. All three had distinct version IDs and complete primary/thumbnail references. The user then created a fourth scene containing four cubes, left without saving, and confirmed it disappeared while the original three stayed correct. The metadata independently retained exactly three creations and three distinct version records.

This supports the correction for the observed save-ID collision. It is not a general rendering fix or recovery of previously missing save files. One unexpected return to Homespace was reported during testing, but did not recur during later checks; no dedicated navigation fix was made.

## Reproduce

Apply the patch after the retained rendering patches, then rebuild. See [SOURCE_BUILD.md](SOURCE_BUILD.md). From a configured clang-cl developer PowerShell, run:

```powershell
& /path/to/Dreams-ShadPS4/tools/tests/random_api/run.ps1 -SourceRoot /path/to/shadps4-source
```

Test saves in a separate portable profile: save two visibly different scenes, close/reopen the emulator, verify both, then create a third scene and discard it. Check that the original records, contents and previews remain intact. Preserve backups before using experimental builds. Existing duplicate-ID records are not repaired by this patch.
