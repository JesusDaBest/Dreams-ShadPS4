# Dreams ordered-count update — October 17, 2026

Exact owner-played executable: `966d8fbe75136e0093d49ad43a85f18c766e6be921c47912ef6b3cb8b3c5be4d`.

Extract **Dreams-ordered-count-20261017.zip**, double-click **Launch-Dreams.cmd**, and enter your own Dreams eboot.bin path. The launcher enables the three required ordered-count settings automatically and retains the preceding tested configuration. Requires your existing game/system setup, Vulkan driver and Microsoft Visual C++ x64 runtime. No game, firmware, saves or captured game resources are included.

## Changes and evidence

This checkpoint adds bounded collect/prefix/replay handling for six additional Dreams ordered-count shaders, together with the preceding scene-compaction and selection-list paths. It retains this source branch's shape, performance and occlusion corrections. The matching cumulative source patch includes the other changes present in the exact executable's source, including opt-in diagnostic code. It does not merge the separate October 6 branch or claim every historical fix is included.

Saved validation reports 108 synthetic full-shader GPU cases comparing 18,881,280 words, with no Vulkan validation errors; 39 existing CPU semantic tests passed and one was skipped. The saved game run activated shader cbac06d2 with 24 workgroups. The owner confirmed the improved visuals were retained. Five additional kernels have synthetic validation without activation evidence in that run. Two other kernels remain excluded. This is not general DS_ORDERED_COUNT support.

The saved frame sample was 14.78 FPS versus 11.59 FPS for a preceding candidate, using different camera views. This does not establish a controlled performance gain. Other scenes and GPUs remain unverified.

Publication checks the running EXE hash, reconstructs 957 project source files from the published base and patch chain, verifies ZIP contents and runs the packaged executable's --help. It does not rebuild the emulator or repeat the saved GPU/in-game tests. See validation.json and source-manifest.json.

## Matching source

Start at public base `b84847d1b6ca14d349bf9e11e91d14ae182e008d` and apply, in order:
- `patches/dreams-source-delta-20261016-performance.patch`
- `patches/dreams-occlusion-array-lod-20261016.patch`
- `patches/dreams-ordered-count-20261017.patch`

Follow SOURCE_BUILD.md for prerequisites. The manifest includes original byte hashes and LF-normalized hashes; patch reconstruction verifies the latter to account for Windows line endings. The supplied launcher enables SHADPS4_DREAMS_SCENE_COMPACT_EXACT_DOC, SHADPS4_DREAMS_SELECTION_LIST_EXACT_DOC and SHADPS4_DREAMS_ADDITIONAL_EXACT_DOC.
