# TODO

Live task list (LID "The task list"). Read at session start; updated the moment anything changes. Not shipped (`export-ignore`).

## In progress
- [ ] Phase 7 round 2 (x86 EPYC 7763, 15 rounds). A/A within ±0.5% (median).
  - `opt/levelscan`: **rejected**. Mixed +0.1% (Clang), +0.3% (GCC); tight −1.7% / −1.3%, overlapping the A/A range. Record in PERFORMANCE.md; delete the branch.
  - `opt/prefetch`: Clang −3.6% mixed, −3.4% tight (every round faster); GCC −0.1% / −0.7% (noise). Both compilers emit the prefetch. Undecided: a Clang-only gain does not help a GCC build.
  - `opt/prefetch2` (ping-pong slots instead of copying the lookahead request; 199/199 tests): 25-round A/B of prefetch vs prefetch2 running, Clang run 36289240780, GCC run 36289242194.

## Up next
- [ ] Further A/B rounds until one produces no keeper; record the stopping evidence and all round results in PERFORMANCE.md.
- [ ] Optional: make `x86-ab.yml` build only matcher/gen_orders/measure (about 10 min per round instead of 25).
- [ ] After the loop: full `scripts/check.sh --stress` in Docker, then one `x86-benchmark.yml` run on final `main`; refresh every PERFORMANCE.md number (plus Arm Linux VM and native macOS if the code changed).
- [ ] Final: full verification, rebuild `dist/order-matcher.zip`, and confirm the zip has no `.cache/`, `CLAUDE.md`, PDF, `docs/TODO.md` (or `docs/` if excluded).
- [ ] User reads README.md, then PERFORMANCE.md, end to end, and asks about anything unclear.
- [ ] Interview-style quiz, level by level: brief and behavior → design choices → performance → testing and robustness → production changes.
- [ ] Reviewer walkthrough: unzip into a clean container, follow the README literally, read the code in the README's order as the Vatic engineer would, and list likely criticisms.
- [ ] User rewrites PERFORMANCE.md in their own voice; Claude checks only technical accuracy and numbers.
- [ ] Optional: S6 long fuzz campaign (`scripts/stress.sh --long` in Docker, about 1 hour).
- [ ] User submits the zip through the recruiter's link.

## Waiting on the user
- [ ] `docs/` in the submission. User leans toward excluding; recommendation is to exclude. Plan: push a `design-docs` branch, then remove `docs/` (except this file), all 203 `// @spec` comments, `scripts/spec_coverage.sh` and its `check.sh` step, the DLV references to it, and the README mentions of `docs/`.
- [ ] Whether `.github/workflows/x86-ab.yml` ships (a dev tool) or is `export-ignore`d. Recommendation: decide with the `docs/` question. `x86-benchmark.yml` ships (it reproduces the published numbers).

## Done
- [x] 2026-09-27: x86 profiles of `main` (runs 36288261726 Intel Xeon 8370C, 36288263475 AMD EPYC 7763): `OrderIndex::find` is the top cost on x86 at 17.5–25.6% (7–9% on the M4). The 32 MB index exceeds these CPUs' L2, so new ids' slots are cold. Other costs: parse 12–16%, reject diagnostics 5–10%, level find 4–11%.
- [x] 2026-09-27: `opt/reserve` merged into `main` (05b14ef, 198/198 tests); `opt/*` branches deleted locally and on GitHub.
- [x] 2026-09-27: Phase 7 round 1 on x86 (runs 36287343098 Clang/AMD EPYC 9V74, 36287344793 GCC/Intel Xeon 8573C). A/A medians within 1.5%. **Keep `opt/reserve`** (−3.0% Clang, −4.0 to −4.3% GCC; faster in every round on Clang). Reject `opt/escape` (−1.3% Clang, but +4% to +11.5% on GCC/Intel), `opt/newline` (noise) and `opt/swar` (+3% to +12%).
- [x] 2026-09-27: `docs/TODO.md` convention added to the private LID fork (v1.2.0, commit 8c965a1) and adopted here.
- [x] 2026-09-27: Phase 7 added to the private LID fork (v1.1.0): https://github.com/hmalladi3/linked-intent-dev
- [x] 2026-09-27: x86 A/B workflow `x86-ab.yml` (interleaved, A/A control); four candidate branches pushed, 198/198 tests each.
- [x] 2026-09-27: Third optimization pass on the M4: four candidates, none kept. The decision is being redone on x86 (above).
- [x] 2026-09-27: x86 benchmark (AMD EPYC 9V45) run; results in PERFORMANCE.md; compiler choice corrected to per-platform.
- [x] 2026-09-26: Passes 1–2: blocked hash + hugepages, parser fast path, flush in `read()`, retained levels, in-place diagnostics.
