# Dreams Development Snapshot

This branch is the complete source snapshot used to run `Dreams` (`CUSA04301`) on August 17,
2026. Fork it or create a new branch before making changes.

## Confirmed state

- Dreams reaches offline menus and creation content.
- Full-screen 3D rendering is working, including flecks, characters, backgrounds, the imp, and UI.
- The clean test build sustained the game's 30 FPS cap on the `Mm Characters` screen.
- The indirect geometry path generated 897 nonzero draw commands, with 504 active instance draws.
- This remains an experimental development build, not a claim that every scene or gameplay path is
  fully playable.

The key rendering corrections include Liverpool workgroup-info SGPR initialization, coherent
thread-mask reads in the Dreams indirect-argument shader, subgroup compaction for sprite records,
real `V_WRITELANE_B32` lane behavior, and removal of the invalid `V_READLANE_B32` mask-shadow
restoration that compressed rendering into the top edge.

## Protect other games

Some changes in this snapshot are emulator-wide and may change compatibility with games other than
Dreams. Downloading the source does not affect existing games, but running the custom build can use
the same saves, settings, trophies, and shader caches as another shadPS4 installation.

For safe testing:

1. Keep the Dreams build in a dedicated folder and use it only for `CUSA04301`.
2. Create an empty folder named `user` in that build's working directory.
3. Launch shadPS4 with the dedicated build folder as its current working directory.
4. Use an official or normal shadPS4 build for other games.

The local `user` folder isolates configuration, saves, trophies, logs, and caches. Game files,
firmware, keys, user configuration, and saves are not included in this repository.

## Investigation notes

Current status, confirmed findings, and priorities are maintained on the repository's `main`
branch:

https://github.com/JesusDaBest/Dreams-ShadPS4
