# Dreams Development Snapshot

This branch is a complete standalone copy of the current patched shadPS4 source for `Dreams`
(`CUSA04301`). Fork it or create a new branch before making changes.

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

Current status, confirmed findings, and priorities are maintained on the repository's `main` branch:

https://github.com/JesusDaBest/Dreams-ShadPS4
