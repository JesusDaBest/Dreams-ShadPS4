# Continue Development

## Easiest option

Fork this GitHub repository, select the `dreams-dev` branch, and create a new branch from it. This
gives each developer an independent copy of the complete current shadPS4 source without affecting
the original investigation branch.

To clone the source snapshot directly:

```bash
git clone --branch dreams-dev --single-branch https://github.com/JesusDaBest/Dreams-ShadPS4.git
cd Dreams-ShadPS4
git switch -c my-dreams-work
```

Then follow the normal shadPS4 build instructions included in that source tree.

## Starting point

- Source branch: `dreams-dev`
- Snapshot commit: `fc3d1b8a526dff7f9edc1c17f4fcc61f220ec954`
- Game serial used for testing: `CUSA04301`
- Current result: startup/tutorial logic runs, but rendering and AV presentation are not fixed
- Technical priorities: [ISSUES.md](ISSUES.md)
- Detailed evidence: [DISCOVERIES.md](DISCOVERIES.md)

## Alternative patch workflow

Developers who already have shadPS4 commit `555c458c9fdd33cb4686492374519c7bb112a891`
can apply `patches/dreams-focused-20260817-current.patch` instead. The branch is recommended because
it avoids base-version and patch-application mistakes.

Game files, firmware, keys, user configuration, and save data are intentionally not included.
