# Continue Development

## Recommended source branch

The `dreams-dev-20260819-experimental` branch is a complete source snapshot of the August 19
investigation state. It includes the same tracked source changes as the cumulative patch, including
unfinished diagnostics and the malformed-geometry candidate.

```bash
git clone --branch dreams-dev-20260819-experimental --single-branch https://github.com/JesusDaBest/Dreams-ShadPS4.git
cd Dreams-ShadPS4
git switch -c my-dreams-work
```

The older `dreams-dev-3d-rendering` branch is retained for comparison, but its August 17 description
overstates the rendering result. Do not use it as evidence that 3D was solved.

## Exact starting state

- Game: `CUSA04301`
- Status: not playable
- UI/imp/grid/gadgets: partially working
- Sculpt and paint/fleck geometry: missing or malformed
- Tweak panels: can be black
- Camera movement: generated geometry can jitter
- Current candidate: restores Vulkan dispatch-base correctness and exposes oversized grey geometry
- Primary blocker: incomplete guest-wave ordering for `DS_ORDERED_COUNT`

Read [STATUS.md](STATUS.md), [DISCOVERIES.md](DISCOVERIES.md), and [ISSUES.md](ISSUES.md) before
changing rendering code. They distinguish confirmed corrections from observations and failed
experiments.

## Patch workflow

Developers with upstream commit `555c458c9fdd33cb4686492374519c7bb112a891` can instead apply:

```bash
git apply --check patches/dreams-focused-20260819-experimental.patch
git apply patches/dreams-focused-20260819-experimental.patch
```

Patch SHA-256:

```text
A7C1863086220579483BA4D4F9DB3E68F40E9B7BF4780D9B1CCF7F036460AEAF
```

The patch was verified with `git apply --check` against that exact base.

## Keep other games isolated

The snapshot contains emulator-wide changes and has not been regression-tested against other
games. Downloading or building it cannot alter other installations, but running it against the same
user directory can share saves, settings, trophies, logs, and caches.

1. Keep the experimental build in a dedicated folder.
2. Create an empty folder named `user` inside that folder.
3. Start shadPS4 with the dedicated folder as its working directory.
4. Use the snapshot only for `CUSA04301`.
5. Keep a verified save backup before any save-data experiment.

No game files, firmware, keys, user configuration, credentials, or saves are included.

## Development discipline

- Change one rendering variable at a time.
- Keep a known scene and camera position for comparisons.
- Record executable/source identity for every screenshot or log.
- Treat nonzero draw commands as evidence, not proof of correct geometry.
- Never call a result fixed until visible output and regression checks pass.
- Keep broad diagnostics disabled during performance measurements.
- Preserve the earlier stable executable separately from experimental candidates.
