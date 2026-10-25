# Normal graphical launcher package — October 19, 2026

[Download the complete package](https://github.com/JesusDaBest/Dreams-ShadPS4/raw/refs/heads/main/builds/cusa04301-graphical-launcher-20261019/Dreams-graphical-launcher-20261019.zip). Extract the entire ZIP and double-click **shadPS4QtLauncher.exe**. Add your own installed Dreams game folder through the normal shadPS4 Qt launcher, select Dreams and click Play. **Dreams patched build (October 19)** is selected automatically. Keep the extracted package together.

This package opens directly through the normal shadPS4QtLauncher.exe. It includes no separate Dreams entry-point program and no command scripts. The bundled core is unchanged: `640a3ba83eba972702d25cbf9a7404134c09075f4ece098881b590dd1714f2ff`. The fixes are compiled into it; users do not need a separate patch activation step. The included adapter applies the tested CUSA04301 configuration and environment when Dreams starts and backs up any different existing per-game configuration. Existing game/system data and saves remain in the normal shadPS4 profile. The Qt launcher has separate local launcher preferences, leaving other installed launchers' selections alone.

## Checks performed

- Observed the real Qt launcher window with Play and the expected selected build.
- Verified the Qt default-core route reaches the included adapter and exact tested core using a dry run, without starting Dreams.
- Verified relocation to a folder containing spaces resets the correct core path.
- Verified settings application, backup, environment filtering and native queue mode using a harmless child executable in an isolated test profile.
- Verified the normal launcher and adapter are Windows GUI executables and the ZIP has no CMD/BAT/PowerShell scripts, generated game paths or personal launcher preferences.
- Verified every packaged file hash and ZIP CRC. See the package manifest and verification.json.

No new in-game rendering or performance test was performed for this packaging update. Emulator scope and limitations remain those of the [October 19 core](SHADER_INSTRUCTIONS_BUILD_20261019.md). This is a Windows x64 package requiring the user's own game/system setup, Vulkan drivers, .NET Framework 4.x and the Visual C++ x64 runtime. Official core updates do not automatically include these experimental fixes.

## Source

The unchanged Qt launcher was extracted from the owner's existing release ZIP. Its embedded source commit is `99e9829e3787031e7eee61935ffd09e8ad8678d1`; [matching launcher source](../builds/cusa04301-graphical-launcher-20261019/qt-launcher-source-99e9829.tar.gz) is included beside the package, with upstream dependency references in .gitmodules. [Qt launcher upstream](https://github.com/shadps4-emu/shadps4-qtlauncher/tree/99e9829e3787031e7eee61935ffd09e8ad8678d1). The automatic settings adapter C# source is included in runtime/ and this build directory. Build it with the .NET Framework C# compiler as winexe/x64, referencing System.Web.Extensions.dll and System.Windows.Forms.dll. The normal Qt launcher selects that adapter through clean portable preferences with relative paths; no separate entry-point program is used. Matching emulator source is linked in the core's build document. Third-party notices and license texts are in the package.
