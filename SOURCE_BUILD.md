# Build the Dreams source fixes

This is the source-patch workflow for the retained rendering checkpoint. You can compile it yourself; the archived executable is an optional historical comparison. These patches are tied to an upstream revision and tested game version, not guaranteed to apply to arbitrary shadPS4 revisions or work on every GPU.

## Verified game and settings

The local game inspected on October 3 reports **CUSA04301, APP_VER 01.00**. The working profile uses **GPU readbacks: Precise** (`GPU.readbacks_mode = 2`). Check the Dreams-specific override as well as the global setting: an override wins. Modes 0 and 1 mean Disabled and Relaxed respectively.

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
cmake -S . -B build-dreams -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-dreams --parallel 1
```

Stop if any command fails. Initialize submodules before applying patches: the cumulative research patch changes the recorded Sirit revision, while the supplied Sirit delta is applied to the base checkout at `282083a595dcca86814dedab2f2b0363ef38f1ec`. Do not run a subsequent submodule reset over the patched Sirit source.

CMake commands describe a standard Release build in a configured developer shell. They were not freshly run for this documentation update; the original checkpoint has a recorded successful build. Compiler and dependency differences can change binary hashes, so equal hashes are not required of independently compiled builds.

Select your newly compiled executable in your launcher, set Dreams readbacks to Precise, and test offline startup, Homespace and stamping a sculpt. Disable diagnostic environment flags. Preserve saves before using experimental source. Keep the effective game version, settings and startup log with any result.

## Optional October 2 save-handling delta

The running local executable identified on October 3 hashes to `DFC6AD73E02CABEEC01DDA48016CAE291ACA40D7C23444BCAA47F4BFFF033E90`. An identical build artifact exists beside the October 2 source. That source's committed tree equals the published August 29 source tree; its only tracked local changes are the two save-handling files exported in [save-mount-lifecycle-20261002.patch](patches/save-mount-lifecycle-20261002.patch).

This delta serializes mount-slot access, checks duplicate saves across all slots with user/title identity, and avoids a fatal assertion when unmounting an inactive instance. It is separate from the rendering corrections and is not a demonstrated remedy for the 2.65 black screen. It passed patch-application and source-equivalence checks here; no new concurrency/save regression or game test was run during this audit.

To reproduce that additional local source delta, apply it after the checkpoint patches and before building:

```powershell
git apply --check "$dreamsPatches/save-mount-lifecycle-20261002.patch"
git apply "$dreamsPatches/save-mount-lifecycle-20261002.patch"
```

## Research patches are separate

The September 2 WIP and September 4/7 research deltas have different bases and validation limits. Do not apply every patch in this repository as one stack. See [DEVELOPMENT.md](DEVELOPMENT.md) for historical research provenance and [Dreams-runtime-setting-fix-20260904.md](Dreams-runtime-setting-fix-20260904.md) for the configuration comparison.
