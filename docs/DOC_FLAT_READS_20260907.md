# Shared DOC: flattened constant reads

Implemented generic fixed-slot ReadConst support in the isolated shared DOC
prototype. It uses the bound flattened userdata snapshot during collect and
replay. It does not substitute hardcoded values or select shaders by game hash.
Dynamic/unflattened reads, invalid slots/resources and DOC-dependent read
addresses remain rejected. The caller must establish snapshot freshness,
stability and nonaliasing; the normal renderer still leaves the prototype off.

Verified results:

- 83 synthetic GPU cases passed, including11 new runtime constant-data cases.
- 94 planner checks passed;10 omitted-prefix controls detected failures and two
  invalid-phase controls preserved all buffers.
- Zero output mismatches and zero Vulkan validation errors.
- All26 recovered real-shader permutations still complete offline replay.
- `252ccde6` and `fff20d00` pass static planning and emit SPIR-V validated for
  Vulkan1.3. This is two real shaders, up from one before this change.

The other24 permutations first reject on extra GDS access(11), UndefU32(5),
workgroup shape(4), loops/early exits(2), or image sampling(2). These are first
rejections, not a complete defect inventory. Four of the26 known DOC shader
bodies still lack cached input metadata.

The real shaders were not GPU-executed, and no gameplay improvement or full
performance claim follows. The installed best emulator remains unchanged.

Evidence and source:

- GPU fixture report (`D:/CodexData/Workspaces/2026-09-04/usi/work/doc-shared-gpu/README.md`; local evidence)
- Real shader replay (`D:/CodexData/Workspaces/2026-09-04/usi/work/doc-input-capture/README.md`; local evidence)
- Isolated source: `D:/CodexData/Worktrees/Dreams-ShadPS4/doc-shared-20260907`

The earlier input-recovery checkpoint is [DOC_SHARED_OFFLINE_20260907.md](DOC_SHARED_OFFLINE_20260907.md).


Publication note: local evidence paths record the original investigation environment. They are not downloadable repository links. Test results above are historical reports, not newly rerun validation.
