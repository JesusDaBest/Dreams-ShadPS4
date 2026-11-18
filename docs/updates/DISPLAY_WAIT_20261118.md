# Display wait fix · November 18, 2026

Dreams can keep processing host commands and compute submissions while waiting for the display to release a buffer. The graphics packet still waits for the original display signal.

In the matching private build, the first two seconds after breaking the wall in Ancient Dangers averaged **28.8 game FPS**, up from **19.6 FPS**. The tested Tren Starter Set view held **30 FPS** with the fix both on and off.

[Download for Windows](https://github.com/JesusDaBest/Dreams-ShadPS4/raw/refs/heads/main/builds/cusa04301-display-wait-20261118/Dreams-graphical-launcher-display-wait-20261118.zip) · [Source patch](../../patches/dreams-display-wait-20261118.patch) · [Source manifest](../../builds/cusa04301-display-wait-20261118/source-manifest.json) · [Verification](../../builds/cusa04301-display-wait-20261118/verification.json)
