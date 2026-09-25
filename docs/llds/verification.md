# Verification

## Context and Design Philosophy

The brief asks for "appropriate … tests" and "all datasets and supporting code used for testing", and the reviewer is a developer who will read them. The tests have to **convince** a skeptical reader, not just pass. Every layer below answers a specific doubt the reviewer might have.

| Reviewer's doubt | Layer that answers it |
|---|---|
| "Does each piece do what it says?" | Unit tests |
| "Does it do exactly what the brief says, byte for byte?" | Golden end-to-end scenarios |
| "What about cases nobody thought of?" | Differential testing against the reference engine, plus invariant checks and property tests |
| "Can input crash it?" | libFuzzer + sanitizers, plus the hostile-input dataset |
| "Does it really not allocate / is it really fast?" | Allocation-counting test + the benchmark matrix |
| "Is every requirement tested?" | EARS traceability check |

Principles:
- **Tests read like the brief.** Scenario tests use a small helper that takes CSV lines in and returns output lines, so a test looks like the brief's own examples.
- **Deterministic.** Every randomized test takes a seed and prints it when it fails. Failing seeds are added to the regression list.
- **Test the shipped code.** Tests and benchmarks link the same `matcher_core` library as the app. There are no test-only code paths in production classes, except `OrderBook::check_invariants()`, which is read-only.

## Layout

```
tests/
  unit/            price_test, line_reader_test, parser_test, node_pool_test,
                   order_index_test, level_store_test, order_book_test,
                   matching_engine_test, event_writer_test, error_reporter_test
  scenario/        brief_examples_test, priority_test, rejection_test, ...
  property/        differential_test, invariants_test, properties_test
  support/         reference_engine.{h,cpp}, capture_sink.h, request_generator.{h,cpp},
                   harness.h (CSV-driven engine), fake_io.h (scripted ByteReader/ByteWriter)
  alloc/           no_alloc_test (own binary: replaces global operator new)
  e2e/             run_golden.cmake (runs the real binary per data/golden case)
fuzz/              fuzz_pipeline.cpp, fuzz_price.cpp (libFuzzer), corpus/ seeded from data/
bench/             bench_main.cpp, scenarios.{h,cpp}, timer.h
tools/             gen_orders.cpp (seeded dataset generator)
data/
  golden/          <name>.in, <name>.out, <name>.err  (committed)
  generated/       small committed samples + README with regeneration commands
scripts/           check.sh, coverage.sh, spec_coverage.sh
```

## Layer 1: Unit Tests (GoogleTest)

Each component's tests are table-driven wherever there are more than three cases.

- **Price:**
  - every accepted and rejected row in `price.md`,
  - the exact boundaries (±92233720368.54775807 accepted, ±92233720368.54775808 rejected, 8 vs. 9 decimal places, trailing zeros),
  - formatting of zero, negatives, and pure fractions,
  - a round-trip property over random raw values.
- **LineReader:**
  - the same input fed through a fake file descriptor in chunk sizes 1, 2, 7, 4096, and 1 MiB must produce identical lines,
  - CRLF endings, a missing final newline, empty input,
  - lines exactly at, one over, and far over 4096 bytes (discarded without storing them, and line numbering stays correct afterwards),
  - NUL bytes, `EINTR` injection, a read error.
- **Parser:** every row of the `protocol.md` table, and every error branch reached at least once, including the trimming, blank-line, and escaping rules.
- **NodePool / OrderIndex / LevelStore:**
  - acquire, release, and reuse order,
  - growth with handles still valid,
  - index collisions forced with ids that map to the same slot,
  - backward-shift deletion in the middle of a cluster and across the table's wrap-around,
  - growth and rehash under load,
  - the level store's fast path vs. binary-search path on both sides.
- **OrderBook:** `fill_best` (partial and full, emptying the level), `rest` (new level at the best price, in the middle, at the worst price), `cancel` (head, middle, and tail of the FIFO; the only order in its level; an unknown id), and `check_invariants` after each.
- **MatchingEngine:** the scenario layer covers behavior. Unit tests cover the rejection codes, including `CapacityExceeded` via a small capacity limit injected at construction.
- **EventWriter / FdWriter / ErrorReporter:**
  - exact bytes for each message,
  - partial `write` and `EINTR` via a fake file descriptor,
  - `EPIPE` sets the sticky failure flag,
  - the flush-before-blocking behavior.

## Layer 2: Scenario Tests (brief semantics)

These are written as `run({"0,1,1,1,1075", ...}) == {"2,2,1025", ...}`.

- **The brief's worked example,** step by step, including the book snapshot at each stage, and the follow-up "a new sell at 1025 queues behind S4".
- **Price priority:** a better price is filled first on both sides. **Time priority:** within a level, the oldest is filled first, and priority survives partial fills.
- **Trade price:** always the resting order's price. A buy above the best ask gets price improvement.
- **Equal prices cross.** A price one unit (10^-8) short of crossing does not match.
- **Sweeps:** across several levels on both sides, stopping at the limit price, leaving a remainder that rests on the book, and filling exactly with no remainder.
- **Sequence:** each matched pair is emitted in the order Trade → aggressive fill → resting fill.
- **Fill messages per trade:** at least one FullyFilled and at most one PartiallyFilled per trade.
- **Id reuse after a fill or cancel:** the new order goes to the back of the queue. **A duplicate live id** is rejected with no side effects, on the same side and the opposite side, including when it would have crossed.
- **Cancels:** the best order, a deep order, the last order in a level (the level disappears and the best price moves), unknown ids, cancelling twice, cancelling a filled id.
- **Extreme prices and quantities:** negative, zero, and extreme prices; `UINT64_MAX` quantities in both directions.

## Layer 3: Golden End-to-End

CTest runs the **real binary** once per `data/golden/<name>.in` and compares stdout and stderr byte-for-byte with `<name>.out` and `<name>.err`, and also checks the exit code.

| Dataset | Purpose |
|---|---|
| `brief_example` | The brief's example as clean CSV, including `BADMESSAGE` |
| `brief_example_verbatim` | The same example exactly as copied from the brief, with its `//` comments and zero-width spaces. It must produce the same stdout. |
| `priority_and_sweeps` | Multi-level sweeps and time priority on both sides |
| `cancels` | Every cancel case |
| `malformed` | Every parse error, one line each |
| `hostile` | Binary garbage, NUL bytes, 1 MB lines, overflowing numbers, CRLF, a missing final newline |
| `negative_prices` | Spreads through zero, and negative trades |
| `id_reuse` | Reuse after a fill and after a cancel |
| `empty` | Empty input |

## Layer 4: Differential + Invariants + Properties

- **ReferenceEngine** (`tests/support`): a deliberately naive design. Each side is a `std::vector` of orders; it scans linearly for the best price and removes with `erase`. It is about 60 lines that can obviously be checked against the brief. It shares **no code** with the real engine except the plain types.
- **RequestGenerator:** seeded, with several profiles:
  - `tight`: a narrow price band, so matching is heavy,
  - `deep`: many levels and few crossings,
  - `cancel_heavy`,
  - `sweep`: large aggressive orders,
  - `id_reuse`,
  - `extreme`: boundary prices and quantities,
  - `mixed`.
  It also injects duplicate and unknown ids on purpose.
- **Bounded books:** the generator caps the set of ids that may still be live (200–2000 depending on the profile) and forces cancels at the cap. That keeps the naive reference engine fast enough for 10^6 requests.
- **Differential test:** for each profile and two fixed seeds, both engines process at least 10^6 requests (10^5 in Debug and sanitizer builds). Event streams and rejections must be identical after every request. Also after every request:
  - `check_invariants()` runs,
  - the event stream's structure is checked (Trade → aggressor fill → resting fill),
  - the book is checked for crossing.
  Full level-by-level snapshot comparison with the reference runs every 128 requests and at the end. Every 128 is plenty to localize a divergence; comparing after every request would dominate the runtime.
- **Default reservations are deliberately tiny** in unit, scenario, and differential tests, so ordinary tests also exercise every growth path.
- **Property checks,** evaluated on every run:
  - quantity is conserved (Σ added = 2·Σ traded + Σ resting + Σ cancelled),
  - every trade is at the price of a resting order that existed,
  - the book is never crossed after a request,
  - the same input always produces identical output.

## Layer 5: Fuzzing + Sanitizers

- **`fuzz_pipeline`:** sends arbitrary bytes through the whole pipeline: `LineReader` (over an in-memory `ByteReader` with data-dependent chunk sizes), then parser, then engine (with a 64-node cap, so `CapacityExceeded` is reachable), then `EventWriter` and `ErrorReporter` (to a null writer). It calls `check_invariants()` after every request.
- **`fuzz_price`:** checks the parse → format → parse round-trip and never-crash properties.
- **Build:** both use Clang with `-fsanitize=fuzzer,address,undefined`. The corpus is seeded from `data/golden`.
- **Runs:** `check.sh` does a 60-second smoke run for each target. There are documented instructions for longer campaigns.
- **Sanitizers for the whole test suite:** the entire suite also runs under ASan + UBSan (`-fno-sanitize-recover=all`) on both GCC and Clang.

## Layer 6: Allocation Test

This is a separate binary that replaces global `operator new`/`delete`, so it can both **count** allocations and **inject allocation failure**.
- **Zero allocations:** the whole hot path (line reader → parser → engine → event writer, plus diagnostics) runs with default reservations. It warms up for 10^4 lines, then processes 10^6 more, and asserts **zero** allocations.
- **The counter works:** a companion case exceeds the reservation and expects allocations to be counted.
- **Injected failure:** growth failing mid-run must produce `CapacityExceeded` with the book unchanged (MATCH-REJ-004). Startup allocation failure must make `run_app` exit 1 with `cannot reserve memory for N orders`, without reading any input (PROTO-APP-006).

## Layer 7: Benchmark

- **Harness:** `bench/` has its own harness, with no third-party library, so the timing method is fully under our control and visible to the reviewer.
- **Timer:**
  - On x86-64, it uses `rdtscp` + `lfence`, calibrated against `steady_clock`.
  - Elsewhere, for example on Apple Silicon development machines, it uses `steady_clock`.
  - The timer's own overhead is measured and reported.
- **Scenarios:**
  - each row of the HLD's benchmark matrix, at 10^3–10^6 resting orders,
  - parse-only, format-only, and end-to-end throughput (the real binary with piped input),
  - a growth-spike scenario that runs without `--reserve`.
- **Output:** a Markdown table (p50, p99, p99.9, max, and mean in ns, plus throughput in messages per second) that is pasted into `PERFORMANCE.md`. The CPU model, compiler, flags, and seed are included.
- **Stability:** warm-up iterations run first, results are the median of 5 runs, and it can be pinned to a core with `taskset`, which is documented.

## Layer 8: Static Quality Gates

- Warnings: `-Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow -Werror` on GCC and Clang.
- `clang-tidy` with a committed `.clang-tidy` (bugprone, performance, modernize, readability subsets).
- `clang-format` with a committed `.clang-format`, checked in `check.sh`.
- **Coverage:** `scripts/coverage.sh` (llvm-cov) reports line and branch coverage for `matcher_core` and fails below the HLD thresholds.
- **Spec traceability:** `scripts/spec_coverage.sh` checks that every EARS ID in `docs/specs/` is cited by at least one `@spec` in `tests/`, and that every cited ID exists.

## `scripts/check.sh` and Docker

- `check.sh` runs Layers 1–8 across {GCC, Clang} × {Debug, Release, ASan+UBSan}, plus fuzz smoke runs, coverage, and a short benchmark.
- It exits non-zero on the first failure, printing a summary.
- The `Dockerfile` (Ubuntu 24.04, GCC 13, Clang 18, CMake, GoogleTest) runs it the same way on any host: `docker build -t matcher . && docker run --rm matcher scripts/check.sh`.

## Decisions & Alternatives

| Decision | Chosen | Alternatives Considered | Rationale |
|---|---|---|---|
| Test framework | GoogleTest | Catch2; doctest; a hand-rolled framework | It is the most widely known in C++ shops, has good parameterized-test support, and is allowed by the brief. |
| Oracle | An independent naive reference engine | Only hand-written expectations | Hand-written cases only cover what someone thought of. A differential test against an obviously correct model covers what nobody thought of. |
| Benchmark library | Custom harness | Google Benchmark | We need per-operation latency percentiles with the book's state controlled. Google Benchmark focuses on mean throughput per loop. A custom harness also shows timing competence, which the JD values. |
| Fuzzer | libFuzzer | AFL++ | It is built into Clang, so no extra install is needed, and it pairs directly with the sanitizers. |
| Golden-file comparison | CMake script running the real binary | A shell `diff` harness | It is portable and runs inside `ctest`, with no extra tooling. |

## Open Questions & Future Decisions

### Deferred
1. A continuous-integration configuration, such as a GitHub Actions workflow. `check.sh` and Docker already let anyone reproduce everything. A CI file could be added if the repo is ever hosted, but that is not required for submission.
2. Running under Valgrind (memcheck). It overlaps with ASan and is slow, so it is optional and documented only.

## References

- `docs/high-level-design.md`: Success Metrics
- The other LLDs; each one's edge cases are covered by Layer 1 and Layer 2 above
