Dreams on shadPS4 - October 29, 2026 depth copy update

Extract the entire ZIP, open shadPS4QtLauncher.exe, add your own Dreams game
folder and click Play. The patched core and matching settings apply automatically.
No command prompt or separate setup script is needed.

This update prevents large depth/colour texture copies from exceeding the
temporary buffer on GPUs using the fallback copy path. All earlier fixes,
including depth-texture initialization and the Tren repair, are retained.

The package uses your normal shadPS4 profile. It contains no games, firmware,
saves or creations. Keep the extracted folder together. Existing portable
profiles are not moved automatically.

If a tutorial blocks progress, close Dreams and open
tutorial-skip/Skip Dreams Tutorial.exe. Select your own CUSA04301/SYSTEM/
savedata.json. The helper makes a backup before changing progression flags.

Some rendering glitches and Tren slowdowns remain; no overall speedup is
claimed. Source changes and checksums accompany this update.
