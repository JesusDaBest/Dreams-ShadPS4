# Faster Dreams emulator development

A correct DS_ORDERED_COUNT implementation and a playable Dreams result are not guaranteed. They remain separate outcomes to establish with evidence. The existing repeatable crash makes this instruction a justified target; it does not prove that it is the last blocker.

The highest-value efficiency improvements are:

1. **Rebuild only affected code.** The previous custom builder combined every header into one fingerprint, so a header change invalidated all 390 source files. Compiler-generated dependency records allow each translation unit to be checked against its actual inputs. Existing cache entries without those records must be rebuilt safely once. Widely included headers can still require a large rebuild.
2. **Keep verified dependencies and the baseline fixed.** Reuse the rebuilt official Sirit library and existing verified inputs. Preserve the tested executable and captured failing shader, so every experiment has an identifiable starting point.
3. **Use small tests for each question.** Validate decoding, register inputs, side effects and per-wave results separately. Run Dreams only to gather missing runtime information or test a meaningful change. Repeatedly reproducing the same uninstrumented crash adds no new evidence.
4. **Keep one shared decision record.** Current facts, known mistakes, missing evidence and the next experiment are in `work/ORDERED_COUNT_STATE.md`. Read that record before repeating broad searches. Give helpers independent tasks and explicit file ownership.
5. **Keep resource use bounded.** Use modest low-priority compilation concurrency, one emulator instance and capped logs. Broad tracing and multiple overlapping game runs are unnecessary for the current blocker.

The next targeted game run should collect the new generic compute diagnostic: dimensions, full/partial thread counts, TG_SIZE/TGID flags, dispatch order bits and COMPUTE_MAX_WAVE_ID. These facts will constrain the implementation and its synthetic tests. It is not yet proven that workgroup order matches the hardware's ordered wave tokens.

The user may be able to obtain PS4 hardware-test results later. Those can resolve uncertain behavior directly; no hardware access is assumed available now. Meanwhile, independently justified parts can be developed and tested without pretending that uncertain behavior is established.

No numeric project speedup or completion date is promised. Build-cache fixture results, when available, measure the cache behavior rather than the speed of completing GPU emulation.

Implemented build-workflow changes: per-source compiler dependency records; separate cache records to avoid repeatedly rewriting all dependency data; content-stable generated files; configurable 1–8 low-priority compiler workers (default 3). The prior driver is preserved as `work/upstream-core-build/build.before-dependency-cache.py`.

Small real-compiler fixtures passed: initial build compiled 2 files; an unchanged run compiled 0; private-header, transitive-header, system-header, `.inc` and flag changes compiled only the affected file. Deleted headers failed safely without caching success. Changed objects and old cache entries were rejected. Updating one record left the other unchanged. These are fixture results, not a measured full-emulator speedup. Results are in build-cache-validation.json (`build-cache-validation.json`; not included in this repository).

The first actual core build with the new cache still needs 390 recompiles once to collect trustworthy dependency records. Resource compilation and linking still run. Force a rebuild after include-search additions/shadowing; keep inputs stable during compilation. See Build-cache-notes.md (`Build-cache-notes.md`; not included in this repository). No full core build or additional game run was performed for this workflow change.

Publication note: local evidence paths record the original investigation environment. They are not downloadable repository links. Test results above are historical reports, not newly rerun validation.
