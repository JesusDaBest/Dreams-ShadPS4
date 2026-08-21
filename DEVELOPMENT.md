# Continue Development

Read [HANDOFF_20260821.md](HANDOFF_20260821.md) first. It records the exact corrected and corrupted
ordered-base states, the CPU scene-cache invariant, unsafe experiments to avoid, and the next
focused trace.

## Recommended source branch

`dreams-dev-20260821-sculpt-handoff` is the complete August 21 research checkpoint:

```bash
git clone --branch dreams-dev-20260821-sculpt-handoff --single-branch https://github.com/JesusDaBest/Dreams-ShadPS4.git
cd Dreams-ShadPS4
git submodule update --init --recursive
git -C externals/sirit apply --check ../../patches/sirit-group-nonuniform-shuffle-20260821.patch
git -C externals/sirit apply ../../patches/sirit-group-nonuniform-shuffle-20260821.patch
git switch -c my-dreams-work
```

The Sirit patch is required because the checkpoint calls `OpGroupNonUniformShuffle` and a dirty
submodule pointer cannot carry its two local source changes.

## Patch workflow

Developers starting at upstream shadPS4 commit
`555c458c9fdd33cb4686492374519c7bb112a891` can apply the cumulative patch instead:

```bash
git apply --check patches/dreams-focused-20260821-sculpt-handoff.patch
git apply patches/dreams-focused-20260821-sculpt-handoff.patch
git submodule update --init --recursive
git -C externals/sirit apply --check ../../patches/sirit-group-nonuniform-shuffle-20260821.patch
git -C externals/sirit apply ../../patches/sirit-group-nonuniform-shuffle-20260821.patch
```

Verified SHA-256 values:

```text
dreams-focused-20260821-sculpt-handoff.patch
43B4407F010D9E34930C035A2C0D15D6D626157B30D6593B2AD9D020D33249B8

sirit-group-nonuniform-shuffle-20260821.patch
F9DBF0B7C8BD4F43EC2104D576C238195F69178082C53C76E55E4C4F5E9C0D06
```

Both patches pass `git apply --check` against their exact bases. The combined source built
successfully as a Release executable.

## Exact starting state

- Game: `CUSA04301` 1.00
- Status: not playable
- UI/imp/grid/gadgets/sky flecks: partially working
- Sculpt and paint/fleck geometry: missing or malformed
- Correct ordered address: corrupted cubes disappear, sculpt may be invisible
- Intentional comparison address: dark/fragmented/flickering cubes
- Current producer blocker: large root never reaches paired CPU record publication
- Cache candidate: sentinel identity + both incremental table sizes zero
- Remaining later blocker: incomplete guest-wave ordering for `DS_ORDERED_COUNT`

## Corrupted-cube A/B

The corrected path is default. To reproduce the earlier weird cubes, set this before shader
compilation and use a fresh shader/pipeline cache:

```text
SHADPS4_DREAMS_REPRO_CORRUPTED_ORDERED_BASE=1
```

Unset it and use another fresh cache for the corrected A/B. The comparison deliberately restores
the wrong formula. Do not build further fixes on top of its output.

## Safe next work

1. Keep guest `+0x8b7bc0` intact and trace the first resource rejection.
2. Confirm cache identity commit at `+0x8b8113` and large-root enumerated count.
3. Confirm the large root contributes type-1 and paired records before changing GPU traversal.
4. If CPU records appear but model output remains zero, follow the model/output builder next.
5. Return to `DS_ORDERED_COUNT` wave release/done semantics only after the producer count is live.

Do not force readiness bytes, invert broad branches, force the published record count, bypass the
resource rejection, or call malformed cube output fixed.

## Keep other games and user data isolated

The checkpoint contains emulator-wide changes and thousands of lines of opt-in tracing. It has not
been regression-tested against other games.

1. Keep the experimental build in a dedicated folder.
2. Use a separate portable `user` directory.
3. Use the snapshot only for `CUSA04301`.
4. Keep a verified save backup before save-data experiments.
5. Disable forced-wait diagnostics for performance measurements.

No game files, firmware, keys, credentials, user configuration, saves, or executables are included.
