# Deliverables Specs

Source: `docs/high-level-design.md` (Goals, Success Metrics) and `docs/llds/verification.md`. These specs describe what the submission must contain. Several are satisfied by an artifact (a document, script, or dataset) rather than by a unit test. The spec-coverage script checks those by the artifact's existence and by `check.sh` running them.

## Build and Run

- [ ] **DLV-BUILD-001**: The project shall build with CMake as C++20 on Linux with GCC 10+ or Clang 12+, with no third-party dependencies outside tests.
- [ ] **DLV-BUILD-002**: The project shall build without warnings under `-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Werror` on both GCC and Clang.
- [ ] **DLV-BUILD-003**: The README shall give single commands to build, run, test, and benchmark the solution, both natively on Linux and through the provided Dockerfile.
- [ ] **DLV-BUILD-004**: Where GoogleTest is not installed on the system, the build shall download it with CMake `FetchContent`.
- [ ] **DLV-BUILD-005**: `scripts/package.sh` shall produce the submission zip from a clean copy of the tree, containing all source, project, test, dataset, and documentation files, and excluding build output and the confidential assignment brief.
- [ ] **DLV-DOC-001**: The README shall open with a reviewer's guide: a one-paragraph summary, an architecture sketch, a file map, and a suggested reading order starting from the matching loop.

## Verification Artifacts

- [ ] **DLV-TEST-001**: The repository shall include every dataset used for testing: golden inputs with expected stdout and stderr, the hostile-input set, and the seeded generator with commands that reproduce any larger dataset.
- [ ] **DLV-TEST-002**: The test suite shall include a naive reference engine and run differential tests against it for every generator profile, with at least 10^6 requests per profile in Release builds, 10^5 in Debug builds and 2×10^4 in sanitizer builds.
- [ ] **DLV-TEST-003**: The randomized tests shall run `check_invariants` and the property checks (quantity conservation, trades at resting price, no crossed book, deterministic output) after every request.
- [ ] **DLV-TEST-004**: The repository shall include libFuzzer targets for the full input pipeline and for price round-tripping, with a corpus seeded from the golden datasets.
- [ ] **DLV-TEST-005**: The full test suite shall pass under ASan and UBSan with `-fno-sanitize-recover=all` on both GCC and Clang.
- [ ] **DLV-TEST-006**: A dedicated test binary shall replace global `operator new` and assert zero allocations over 10^6 mixed requests within reserved capacity (10^5 in sanitizer builds).
- [ ] **DLV-TEST-007**: Library coverage shall be at least 95% of lines and 90% of branches, with every parser error branch covered.
- [ ] **DLV-TEST-008**: Every behavioral spec ID in `docs/specs/` shall be cited by at least one `@spec` annotation in `tests/`, and every cited ID shall exist. `scripts/spec_coverage.sh` checks both.
- [ ] **DLV-TEST-009**: `scripts/check.sh` shall run the build matrix of {GCC, Clang} × {Debug, Release, ASan+UBSan}, all tests, fuzz smoke runs, coverage, static checks, and a short benchmark, and shall exit non-zero on any failure.

## Performance Evidence

- [ ] **DLV-PERF-001**: The benchmark shall report p50, p99, p99.9, max, and mean latency for every scenario in the HLD benchmark matrix at 10^3, 10^4, 10^5, and 10^6 resting orders, plus parse-only, format-only, and end-to-end throughput.
- [ ] **DLV-PERF-002**: The benchmark shall record the CPU model, compiler, flags, and seed alongside its results, and the numbers shall be reproducible with one documented command.
- [ ] **DLV-PERF-003**: `PERFORMANCE.md` shall explain the cost of match detection, filled-order removal, and cancel removal, which request paths are favored and why, the trade-offs made, and the production enhancements (stretch goals 1–3, seeded hashing, binary protocols, kernel bypass, core pinning, and scaling by sharding instruments across cores), citing benchmark results for both GCC and Clang.
