Dreams on shadPS4 - normal launcher with patched core

Extract the entire ZIP and double-click shadPS4QtLauncher.exe.
Add your own Dreams game folder in the normal launcher, select Dreams, and click Play.
Dreams patched build (October 19) is already selected.

The patched emulator core and automatic settings adapter are inside runtime/.
Keep the package together. No separate Dreams application, command prompt,
CMD file, PowerShell script or patch activation is needed.
The emulator fixes are compiled into the included core (SHA256 640a3ba83eba972702d25cbf9a7404134c09075f4ece098881b590dd1714f2ff).
The tested CUSA04301 settings are applied automatically when Play starts Dreams.
Different existing per-game settings are backed up. Existing saves and system
setup stay in the normal shadPS4 profile; launcher preferences are local.

Requires your own Dreams and system setup, Windows x64, Vulkan drivers,
.NET Framework 4.x and Visual C++ x64 runtime. Close Dreams normally before
reopening it. Official core updates do not automatically contain these fixes.
No game, firmware, keys, saves or personal launcher settings are bundled.
Source, evidence and limitations: https://github.com/JesusDaBest/Dreams-ShadPS4
