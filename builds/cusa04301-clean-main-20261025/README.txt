Dreams on shadPS4 - October 25, 2026 clean main build

Extract the whole ZIP. Open shadPS4QtLauncher.exe, add your own Dreams game
folder, select Dreams and click Play. The patched core and matching settings
are selected automatically, just like the preceding graphical updates.
No command prompt, command script, Codex, Python or Builder Bridge is needed.

Keep the extracted folder together. Your own game/system setup is required.
This package uses the normal %APPDATA%\shadPS4 profile. It contains no user
folder, creations, saves, firmware, game content, keys or account information.
The launcher backs up a different per-game configuration before applying its
tested settings. It does not replace or import your saves. Existing portable
profiles are not moved automatically; this is the same profile policy as the
October 21 graphical package.

The exact core is the author's current clean main executable (35cb8a3c).
It includes the ambient-shadow cache correction and earlier texture-size,
shader, sculpt and rendering changes. Custom debug panels, debug connection,
capture tools and Builder Bridge are excluded. Background control and runtime
recorders are disabled; ordinary emulator diagnostic code remains dormant.

The board's colours returned in the tested Tren scene. The owner has also
reported small shadows that can disappear at some viewing angles; that issue
is still unresolved. Other rendering glitches and Tren lag remain. No overall
performance gain, complete compatibility or other-GPU fix is claimed.

Optional tutorial bypass
If a tutorial blocks progress, run Dreams once to create its own profile, close
Dreams normally, then open tutorial-skip/Skip Dreams Tutorial.exe. Select your
own CUSA04301/SYSTEM/savedata.json and confirm the change. It unlocks the tested
progression flags and makes an exact backup beside that file, preserving other
data. To undo, close Dreams and restore that backup. Unsupported save formats
are refused. It never installs another player's save or creates a missing one.

Source, checksums, validation and known issues are in the GitHub update record:
https://github.com/JesusDaBest/Dreams-ShadPS4/blob/main/CLEAN_MAIN_BUILD_20261025.md
