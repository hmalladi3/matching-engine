# TODO

Live task list (LID "The task list"). Read at session start; updated the moment anything changes. Not shipped (`export-ignore`).

## In progress
- [ ] Reviewer walkthrough in progress: the zip is extracted at `~/Downloads/order-matcher` (106 files). Unzip, follow the README literally, read the code in the README order as the Vatic engineer would, and list likely criticisms.
  - Fixed so far: README full-check duration; self-trade prevention spelled out in PERFORMANCE.md; comments that pointed at `order-book.md` (order_book.h ×2, order_index.cpp); dev-only traceability block in check.sh stripped from the zip; leak guard covers excluded file names.
  - Reviewed: README + build (bare Ubuntu and Docker), matching_engine.h, order_book.h/.cpp. Next: level_store, node_pool, order_index, app.cpp, tests. Re-extract `~/Downloads/order-matcher` at the end.
- [ ] User reads README.md, then PERFORMANCE.md, end to end, and asks about anything unclear.
- [ ] Interview-style quiz, level by level: brief and behavior → design choices → performance → testing and robustness → production changes.
- [ ] User rewrites PERFORMANCE.md in their own voice; Claude checks only technical accuracy and numbers.
- [ ] Optional: S6 long fuzz campaign (`scripts/stress.sh --long` in Docker, about 1 hour).
- [ ] User submits the zip through the recruiter's link.

## Waiting on the user

## Done
- [x] 2026-09-27: The submission is `submission/order-matcher.zip` (gitignored), built by `scripts/package.sh`. It leaves out `docs/`, `spec_coverage.sh`, `package.sh` and `.gitattributes` (export-ignore), and strips `@spec` annotations and inline IDs from the zipped copy only; the script refuses the zip if any ID remains. Ships all three workflows, since PERFORMANCE.md cites the A/B. Verified in a clean container: format clean, no warnings, 199/199 tests.
- [x] 2026-09-27: Final verification on the fixed commit. Passed: 6-config build matrix (GCC and Clang × Debug/Release/ASan+UBSan, 199/199 each), fuzz smoke, coverage (98.35% lines, 93.83% branches, parser 100%), clang-tidy, clang-format, traceability 112/112, quick bench, stress suite (S1–S5 all PASS; S1 13.3M msg/s on the Arm VM). Numbers refreshed on the final code: x86 by runner CPU (EPYC 7763 ×3: 10.0–10.3M msg/s Clang; 9V74: 11.7–12.0M; 9V45 earlier: 14.3–14.6M), Arm VM 16.0–17.2M, macOS 16.4–17.6M. README headline is now "10–17 million".
- [x] 2026-09-27: Final `check.sh --stress` found signed overflow (UB) in `fast_price` on 19+ digit prices (GCC UBSan, `RequestParser.FastPathHandlesEveryDigitCount`). Fixed with unsigned accumulators; full suite rerunning.
- [x] 2026-09-27: Phase 7 loop **stopped**: round 1 kept the capacity-check fast path; round 2 (prefetch lookahead, level scan) found no keeper. Prefetch helped on AMD EPYC 7763 with Clang only (−3.5%), was +0.9% on Intel 8370C with Clang, and was noise with GCC. Recorded in PERFORMANCE.md; opt/* branches deleted. (runs 36288506167, 36288507825, 36289240780, 36289242194.)
- [x] 2026-09-27: x86 profiles of `main` (runs 36288261726 Intel Xeon 8370C, 36288263475 AMD EPYC 7763): `OrderIndex::find` is the top cost on x86 at 17.5–25.6% (7–9% on the M4). The 32 MB index exceeds these CPUs' L2, so new ids' slots are cold. Other costs: parse 12–16%, reject diagnostics 5–10%, level find 4–11%.
- [x] 2026-09-27: `opt/reserve` merged into `main` (05b14ef, 198/198 tests); `opt/*` branches deleted locally and on GitHub.
- [x] 2026-09-27: Phase 7 round 1 on x86 (runs 36287343098 Clang/AMD EPYC 9V74, 36287344793 GCC/Intel Xeon 8573C). A/A medians within 1.5%. **Keep `opt/reserve`** (−3.0% Clang, −4.0 to −4.3% GCC; faster in every round on Clang). Reject `opt/escape` (−1.3% Clang, but +4% to +11.5% on GCC/Intel), `opt/newline` (noise) and `opt/swar` (+3% to +12%).
- [x] 2026-09-27: `docs/TODO.md` convention added to the private LID fork (v1.2.0, commit 8c965a1) and adopted here.
- [x] 2026-09-27: Phase 7 added to the private LID fork (v1.1.0): https://github.com/hmalladi3/linked-intent-dev
- [x] 2026-09-27: x86 A/B workflow `x86-ab.yml` (interleaved, A/A control); four candidate branches pushed, 198/198 tests each.
- [x] 2026-09-27: Third optimization pass on the M4: four candidates, none kept. The decision is being redone on x86 (above).
- [x] 2026-09-27: x86 benchmark (AMD EPYC 9V45) run; results in PERFORMANCE.md; compiler choice corrected to per-platform.
- [x] 2026-09-26: Passes 1–2: blocked hash + hugepages, parser fast path, flush in `read()`, retained levels, in-place diagnostics.
