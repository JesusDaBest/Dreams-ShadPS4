# Build the Dreams source fixes

## November 21 Quest VR

Apply the ordered patches in the [emulator source manifest](../builds/cusa04301-quest-vr-20261121/source-manifest.json). For the launcher, apply [the VR menu patch](../builds/cusa04301-quest-vr-20261121/qt-dreams-vr.patch) to the November 18 Qt source archive. The Windows adapter finds SteamVR using the registered OpenVR runtime path.

## November 18 gameplay fixes

Use the base commit and patches in the [source manifest](../builds/cusa04301-gameplay-fixes-20261118/source-manifest.json). The last patch adds the [gameplay fixes](../patches/dreams-gameplay-fixes-20261118.patch). [The Qt launcher uses the same source](../builds/cusa04301-qt-render-mode-20261118/source-manifest.json).

## November 18 Qt rendering setting

The [source manifest](../builds/cusa04301-qt-render-mode-20261118/source-manifest.json) links the emulator source and identifies the Qt launcher revision. Use the [Qt patch](../builds/cusa04301-qt-render-mode-20261118/qt-force-60hz-rendering.patch) or download the [Qt launcher source](../builds/cusa04301-qt-render-mode-20261118/QtLauncher-source-99e9829.tar.gz). [Update details](updates/QT_RENDER_MODE_20261118.md).

## November 18 display-wait build

Apply the patches in the [source manifest](../builds/cusa04301-display-wait-20261118/source-manifest.json). The last patch is [`dreams-display-wait-20261118.patch`](../patches/dreams-display-wait-20261118.patch). [Update details](updates/DISPLAY_WAIT_20261118.md).

## November 17 physics read-range build

Apply the patches in the [source manifest](../builds/cusa04301-physics-read-range-20261117/source-manifest.json) to the base commit in that manifest. The last patch is [`dreams-physics-read-range-20261117.patch`](../patches/dreams-physics-read-range-20261117.patch). [Update details](updates/PHYSICS_READ_RANGE_20261117.md).

## November 16 guarded write-bounds build

Use the [complete source manifest](../builds/cusa04301-proven-write-bounds-20261116/source-manifest.json) and apply its patches in order through `dreams-proven-write-bounds-20261116.patch`. The new patch reconstructs all changed files exactly. Full fresh-build reproducibility remains unestablished. [Update details](updates/PROVEN_WRITE_BOUNDS_20261116.md).

## Earlier November 16 physics build

Use the [complete source manifest](../builds/cusa04301-physics-bounds-20261116/source-manifest.json) and apply its patches in order through `dreams-gpu-proof-physics-bounds-20261116.patch`. The new patch reconstructs all changed files exactly. Full fresh-build reproducibility remains unestablished. [Update details](updates/PHYSICS_BOUNDS_20261116.md).

## Current November 2 build

Use the [current source manifest](../builds/cusa04301-single-mip-write-20261102/source-manifest.json): check out public base `b84847d1b6ca14d349bf9e11e91d14ae182e008d`, initialize its submodules, and apply every listed patch through `dreams-single-mip-write-20261102.patch` in order. Both changed files reconstruct exactly; all earlier published fixes are retained. Full fresh-build reproducibility has not been established. See [the update record and remaining visual issue](updates/SINGLE_MIP_WRITE_20261102.md).

## Earlier source instructions

This is the source-patch workflow for the retained rendering checkpoint. You can compile it yourself; the archived executable is an optional historical comparison. These patches are tied to an upstream revision and a specific tested game installation, not guaranteed to apply to arbitrary shadPS4 revisions or work on every GPU.

## October 15 shape-table correction

The [shape fix record](SHAPE_DENSITY_FIX_20261015.md#source) supplies a focused compiler patch,
a matching project-source delta, an exact base commit and source hashes for the new tested EXE.
Follow that separate recipe for the October 15 build. The older patch stack below is retained
for the earlier checkpoints and is not the recipe for reproducing the new build.

## October 4 selector reset correction

The [selector reset correction](SELECTOR_RESET_FIX_20261004.md) preserves two real Dreams compute dispatches incorrectly skipped as metadata clears. Apply `dreams-selector-reset-fix-20261004.patch` after the focused patches below, or use the latest cumulative diagnostic snapshot described in the validation record. Source compilation is optional; the latest tested executable is linked on the front page.

## October 4 imp/smush correction

The later [sculpt alignment correction](SCULPT_ALIGNMENT_FIX_20261004.md) fixes zero-offset `V_ALIGNBIT_B32` and `V_ALIGNBYTE_B32` translation. The build commands below include its focused patch. The new downloadable diagnostic executable and cumulative source snapshot are described in that record; the focused patch workflow below does not reproduce all diagnostic instrumentation in that executable.

The [raw-image cache refresh](IMP_SMUSH_FIX_20261004.md) was user-tested and removes imp trails and the tweak-menu smush. The build commands include this patch. Other sculpt and tweak-menu rendering issues remain.

## October 3 save-ID correction

The [random-byte correction](SAVE_ID_FIX_20261003.md) passed an isolated three-scene persistence and unsaved-scene discard test. The commands below include it. It does not recover damaged saves or resolve the remaining sculpt rendering defects. The optional October 2 save-mount delta below was also present in the tested executable.

## Game metadata and verified settings

The active launcher command points to the inspected game folder. Its SFO reports **CUSA04301, VERSION 02.64, APP_VER 01.00**. The content update is identified here as **2.64**; the separate APP_VER field alone does not identify the content update. The reported Wide Calculator is consistent with this later version. The earlier conclusion that the installed game was original 1.00 was incorrect. The working profile uses **GPU readbacks: Precise** (`GPU.readbacks_mode = 2`). Check the Dreams-specific override as well as the global setting: an override wins. Modes 0 and 1 mean Disabled and Relaxed respectively.

The September 4 isolated comparison restored the patterned cube with Precise readbacks. This does not establish a universal fix. Sculpts still look wrong.

Dreams **02.65 is not validated** by this checkpoint. Its reported log shows both scene-gate and slowdown-guard byte checks failing. The source explicitly targets the 1.00 executable; do not remove those byte checks or transplant offsets into 2.65. A 2.65 port requires analyzing its executable and validating equivalent behavior. A failure to match establishes that the patches were skipped, not the cause of its black screen.

## Apply and build

Use a new source checkout with Git, CMake, Ninja and the upstream Windows compiler/dependency prerequisites. In PowerShell, replace the first path with your clone of this investigation repository:

```powershell
$dreamsPatches = "D:\path\to\Dreams-ShadPS4\patches"
git clone https://github.com/shadps4-emu/shadPS4.git shadps4-dreams-source
Set-Location shadps4-dreams-source
git checkout --detach 555c458c9fdd33cb4686492374519c7bb112a891
git submodule update --init --recursive
git apply --check "$dreamsPatches/dreams-focused-20260829-full-covered-filled.patch"
git apply "$dreamsPatches/dreams-focused-20260829-full-covered-filled.patch"
git -C externals/sirit apply --check "$dreamsPatches/sirit-group-nonuniform-shuffle-20260829.patch"
git -C externals/sirit apply "$dreamsPatches/sirit-group-nonuniform-shuffle-20260829.patch"
git apply --check "$dreamsPatches/host-entropy-random-20261003.patch"
git apply "$dreamsPatches/host-entropy-random-20261003.patch"
git apply --check "$dreamsPatches/dreams-raw-image-clear-cache-refresh-20261004.patch"
git apply "$dreamsPatches/dreams-raw-image-clear-cache-refresh-20261004.patch"
git apply --check "$dreamsPatches/generic-align-zero-offset-fix-20261004.patch"
git apply "$dreamsPatches/generic-align-zero-offset-fix-20261004.patch"
cmake -S . -B build-dreams -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-dreams --parallel 1
```

Stop if any command fails. Initialize submodules before applying patches: the cumulative research patch changes the recorded Sirit revision, while the supplied Sirit delta is applied to the base checkout at `282083a595dcca86814dedab2f2b0363ef38f1ec`. Do not run a subsequent submodule reset over the patched Sirit source.

CMake commands describe a standard Release build in a configured developer shell. They were not freshly run for this documentation update; the original checkpoint has a recorded successful build. Compiler and dependency differences can change binary hashes, so equal hashes are not required of independently compiled builds.

Select your newly compiled executable in your launcher, set Dreams readbacks to Precise, and test offline startup, Homespace and stamping a sculpt. Disable diagnostic environment flags. Preserve saves before using experimental source. Keep the effective game version, settings and startup log with any result.

## Optional October 2 save-handling delta

The running local executable identified on October 3 hashes to `DFC6AD73E02CABEEC01DDA48016CAE291ACA40D7C23444BCAA47F4BFFF033E90`. An identical build artifact exists beside the October 2 source. That source's committed tree equals the published August 29 source tree; its only tracked local changes are the two save-handling files exported in [save-mount-lifecycle-20261002.patch](../patches/save-mount-lifecycle-20261002.patch).

This delta serializes mount-slot access, checks duplicate saves across all slots with user/title identity, and avoids a fatal assertion when unmounting an inactive instance. It is separate from the rendering corrections and is not a demonstrated remedy for the 2.65 black screen. It passed patch-application and source-equivalence checks here; no new concurrency/save regression or game test was run during this audit.

To reproduce that additional local source delta, apply it after the checkpoint patches and before building:

```powershell
git apply --check "$dreamsPatches/save-mount-lifecycle-20261002.patch"
git apply "$dreamsPatches/save-mount-lifecycle-20261002.patch"
```

## Research patches are separate

The September 2 WIP and September 4/7 research deltas have different bases and validation limits. Do not apply every patch in this repository as one stack. See [DEVELOPMENT.md](DEVELOPMENT.md) for historical research provenance and [Dreams-runtime-setting-fix-20260904.md](Dreams-runtime-setting-fix-20260904.md) for the configuration comparison.

Latest update: [parallel DOC prefix](updates/DOC_PREFIX_20261112.md); use the [complete source manifest](../builds/cusa04301-doc-prefix-20261112/source-manifest.json).

Latest update: [guarded ordered DOC](updates/ORDERED_GUARDS_20261113.md); use its [complete source manifest](../builds/cusa04301-ordered-guards-20261113/source-manifest.json).
