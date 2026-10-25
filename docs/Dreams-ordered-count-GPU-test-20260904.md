# Dreams: production ordered-count GPU test — September 4, 2026

The existing B1 collect → prefix → replay implementation passed a small GPU test on this PC. This is new execution evidence for an existing implementation, not a new game fix. Dreams' black/jittery sculpts and severe lag remain unresolved.

## What ran

The standalone fixture uses the current research tree's **actual production SPIR-V lowering** for B1 (`EmitDataOrderedCount`, hash selector `4ebeffd2`) and the **unaltered production `dreams_ordered_prefix.comp`** shader. Synthetic post-resource-tracking IR supplies known inputs; no game shader is claimed to have been executed.

- AMD Radeon 8060S; subgroup size 64 explicitly required and capability checked.
- 24 workgroups × 64 invocations, GDS counter dword `0x79` initially 17, scratch base 16384 and stride 4.
- Collect and replay use production specialization ID 0, values 1 and 2.
- Six patterns: all zero, one per wave, 64 per wave, varied zero-to-64 counts, their reversal, and sparse contributions at the beginning/middle/end.
- Full buffer comparisons after each phase cover every reservation returned to every lane, the final counter, stored counts/tokens, unchanged inputs, untouched memory and exactly-once per-invocation output writes.

## Results

All six cases passed **381,456 dword comparisons** without a mismatch. The final counters were 17, 41, 1553, 601, 601 and 89 respectively.

Omitting the prefix pass deliberately caused **1,586 mismatches**, which the same checker detected. Both modules passed the SDK's independent SPIR-V validator before GPU execution.

Vulkan validation reported **0 errors and 3 warnings**. The warnings concern duplicate OBS hook registration and two Overwolf layers advertising Vulkan 1.2 while the fixture requests 1.3; they are retained in the evidence log.

The seven-case GPU fixture, including device/pipeline setup and readbacks, took **0.319 seconds wall time**. This is not a GPU timestamp measurement or a Dreams performance claim. The first dependency build compiled 79 small production/stub/Sirit units in 55.2 seconds with three low-priority workers; subsequent builds reuse objects only after checking compiler dependencies and hashes.

An initial harness-only crash was corrected before the successful run: empty inline descriptors tried to read an absent synthetic user-data buffer. Nonempty fixture descriptors resolved it. Production emulator code was not changed for this test.

## What this establishes, and what remains

The tested production lowering and prefix shader agree with the fixture's **stipulated workgroup-index order** on this GPU. This is a useful, fast test platform for further implementation work.

It does not establish PS4 wave-creation/token order or result-lane semantics. The production prefix shader walks workgroup indices and does not use stored tokens to determine order. Full active waves and synthetic tokens were supplied here.

It also does not yet test the first failing `cbac06d2` shader's two ordered-count operations, the guest decoder/resource tracking, original shader eligibility, or reservation-dependent compacted output addressing and alias safety. Output hit counters cover fixture invocations, not actual Dreams output records.

The standalone harness dispatches the real prefix GLSL directly. It does not invoke the production host helper. Separate submissions and fence waits between phases provide stronger synchronization than the live renderer, so this does not validate production barriers or scratch reuse across game dispatches. Bounded input-dependent arithmetic checks repeated input computation; it does not force or measure different wave arrival orders.

## Recommended next step

Keep pursuing genuine fixes using this fast test platform. Extend it to two independent reservations and compare the real `cbac06d2` shader's dependencies and side effects before adapting the execution path. Only then use a bounded Dreams launch to test whether that specific change advances the game. Treat later rendering defects as separate failures to isolate, rather than assuming ordered count explains them all.

Physical PS4 measurements would still strengthen the ordering and lane-behavior evidence, but these bounded implementation tests can proceed now.

## Reproduce and preserve

Original harness directory: `D:\CodexData\Workspaces\2026-09-04\usi\work\ordered-count-gpu`.

Run `build.cmd`, then `C:\Users\Jdura\AppData\Local\Programs\Python\Python312\python.exe run.py` from that directory. The builder uses the existing research checkout and local compiler/Vulkan SDK paths. The ZIP includes the harness, shader modules, executable, logs and build/run provenance; it is an evidence bundle, not a complete standalone source distribution of shadPS4. Each GPU fence has a five-second limit; the runner also bounds its subprocesses.

No emulator was launched or installed for this fixture. The selected restored August 29 core remains SHA256 `183D9914A395D8AD804428B418E13474D71172A6146386DD9E7D159F26AE14CC`. No settings or saves were changed.

Fixture executable SHA256: `cd273393442756a43dda6fda15f4674578cdfff32e6280d4d2c0735fda642ab8`.
Production-lowered SPIR-V SHA256: `eb3025c4d1726e2777b10727ab6b77cde5527f5131437f86bc1183ba30ee3b41`.
Production prefix SPIR-V SHA256: `d15b7cf3ab2a5d11f8216595a122e8f96cf3c8615539b02f8a61f36329e8bce4`.
