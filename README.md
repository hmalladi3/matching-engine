# Order Matcher

<!-- @spec DLV-BUILD-003, DLV-DOC-001 -->

A single-instrument limit order book and matching engine in C++20. It reads add/cancel requests as CSV on stdin, matches them by price-time priority, and writes trades and fill updates to stdout. Every malformed or invalid request is reported on stderr and skipped; no input can crash it.

The core is built for low latency. The book never allocates after startup and never does a system call per message. Matching a crossing order, removing a filled order, and cancelling an order are each O(1). See [PERFORMANCE.md](PERFORMANCE.md) for measurements and trade-offs.

## Reviewer's guide

```
 stdin ─► LineReader ─► parse_request ─► MatchingEngine<EventWriter> ─► EventWriter ─► BufferedWriter ─► stdout
          (1 MiB buffer,  (CSV → AddOrder/     │  price-time rules          (CSV formatting,   (64 KiB, flushed
           bounded lines)  CancelOrder/Error)  ▼                            no allocation)      before blocking)
                               │           OrderBook
                               │            ├─ LevelStore<Buy>/<Sell>  sorted vector, best price at back()
                               │            ├─ NodePool                preallocated nodes; per-level circular FIFO
                               │            └─ OrderIndex              open-addressing id → node
                               └──────────► ErrorReporter ─► stderr   ("line N: reason: excerpt")
```

**Suggested reading order (about 15 minutes):**

1. [`include/matcher/matching_engine.h`](include/matcher/matching_engine.h) implements the matching rules in about 40 lines. Start here.
2. [`include/matcher/order_book.h`](include/matcher/order_book.h) and [`src/order_book.cpp`](src/order_book.cpp) cover book storage and why each operation is O(1).
3. [`src/level_store.cpp`](src/level_store.cpp), [`src/node_pool.cpp`](src/node_pool.cpp) and [`src/order_index.cpp`](src/order_index.cpp) are the three data structures underneath.
4. [`src/app.cpp`](src/app.cpp) is the program loop: read, parse, match, report, and flush before blocking.
5. [`tests/scenario/brief_example_test.cpp`](tests/scenario/brief_example_test.cpp) is the assignment's worked example as a test. [`tests/property/differential_test.cpp`](tests/property/differential_test.cpp) checks the engine against a naive reference on millions of random requests.

**File map**

| Path | Contents |
|---|---|
| `include/matcher/`, `src/` | The library: everything except `main` |
| `app/main.cpp` | Process setup only: ignores SIGPIPE and connects fds 0/1/2 to `run_app` |
| `tests/unit/`, `tests/scenario/`, `tests/property/` | GoogleTest suites (see [Testing](#testing)) |
| `tests/alloc/` | Replaces global `operator new` to prove zero allocations and to inject allocation failure |
| `tests/support/` | Naive reference engine, seeded request generator, test harness, scripted I/O fakes |
| `tests/e2e/`, `data/golden/` | The real binary run on hand-verified input/stdout/stderr datasets |
| `fuzz/` | libFuzzer targets for the whole pipeline and for price round-trips |
| `bench/` | Latency and throughput benchmark |
| `tools/` | `gen_orders` (seeded dataset generator), plus `measure` and `slow_reader` for the stress suite |
| `scripts/` | `check.sh` (every quality gate), `stress.sh`, `coverage.sh`, `spec_coverage.sh`, `package.sh` |
| `docs/` | Design: [high-level design](docs/high-level-design.md), per-component designs in `docs/llds/`, numbered requirements in `docs/specs/` |

Code and tests carry `// @spec ID` annotations that link them to the numbered requirements in `docs/specs/`. `scripts/spec_coverage.sh` checks that every requirement is tested.

## Build and run

**Requirements:** Linux, CMake ≥ 3.20, and GCC ≥ 10 or Clang ≥ 12. Tests use GoogleTest: the system copy (`libgtest-dev`) if installed, otherwise CMake downloads it. The shipped code has no third-party dependencies.

```sh
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release
cmake --build build/release -j

# Run on the assignment's example
build/release/matcher < data/golden/brief_example.in
# 2,2,1025
# 4,1000008,1
# 3,1000005
# 2,1,1025
# 3,1000008
# 4,1000007,4
# (stderr) line 6: Unknown message type: BADMESSAGE

# Test (unit, scenario, differential, allocation, and golden end-to-end tests)
ctest --test-dir build/release -j

# Benchmark (full run takes a few minutes; --quick takes seconds)
build/release/bench/matcher_bench --quick

# Everything: GCC and Clang × Debug/Release/ASan+UBSan, fuzzing, coverage, clang-tidy, clang-format, traceability
scripts/check.sh              # add --stress to also run the stress suite below

# Stress suite only (real binaries under large, sustained and hostile load)
scripts/stress.sh             # --long adds 30-minute fuzz campaigns
```

**With Docker** (any host; the image has GCC 13, Clang 18, libFuzzer and the LLVM tools):

```sh
docker build -t order-matcher .
docker run --rm order-matcher                                       # runs scripts/check.sh
docker run --rm -i order-matcher build/release/matcher < data/golden/brief_example.in
```

**Command line:** `matcher [--reserve N] [--help]`. `--reserve N` preallocates capacity for N resting orders (default 1,048,576). The book still grows beyond it if needed.

## Behavior

**Input.** One request per line:

- `0,orderid,side,quantity,price` adds an order.
- `1,orderid` cancels one.

`orderid` and `quantity` are integers from 1 to 2^64−1. `side` is `0` (buy) or `1` (sell). Beyond the brief's minimum, the parser also accepts:

- `//` comments, so the brief's annotated example can be pasted verbatim, and blank lines.
- Spaces and tabs around fields.
- The invisible Unicode characters that come along when text is copied from documents (the brief's PDF contains zero-width spaces in its example).
- CRLF line endings.

**Output.** For each pair of matched orders, in the order resting orders are matched:

1. `2,quantity,price` (TradeEvent)
2. The aggressive order's `3,orderid` (fully filled) or `4,orderid,remaining` (partially filled)
3. The resting order's fill message, in the same format

**Errors.** Each rejected line produces one diagnostic on stderr, `line N: <reason>: <line>`. Examples:

```
line 6: Unknown message type: BADMESSAGE
line 9: invalid price '1.000000001': more than 8 decimal places: 0,1,0,9,1.000000001
line 12: duplicate orderid 5 (an order with this id is still resting): 0,5,1,1,11
line 14: cannot cancel orderid 77: no resting order with this id: 1,77
```

Non-printable bytes are escaped as `\xHH`. Lines longer than 4096 bytes are reported and skipped without being buffered.

**Exit status:**
- 0: all input processed, even if some lines were rejected.
- 1: an I/O failure, such as stdout closed.
- 2: bad command line.

**Interpretations of the brief** (each is a numbered spec in `docs/specs/`):

| Question | Decision |
|---|---|
| Price representation | Exact fixed-point with 8 decimals (covers every real futures tick, e.g. 1/256). Prices that would need rounding are rejected, never rounded. Printed in shortest exact form (`1025.50` → `1025.5`). |
| Zero and negative prices | Accepted. The brief requires only quantities to be positive, and futures such as calendar spreads (and WTI crude in April 2020) trade at or below zero. |
| Duplicate order ids | An add whose id is currently resting is rejected. Once an order is filled or cancelled its id may be reused; the new order gets fresh time priority. Rejecting every id ever seen would need unbounded memory. |
| Cancel of a filled or unknown id | Rejected with a diagnostic; the book is unchanged. |
| Acknowledgements | Successful adds and cancels print nothing. The brief defines only the three output messages. |
| Partially filled resting orders | Keep their place in the queue. |

## Testing

| Layer | What it shows |
|---|---|
| Unit tests (12 suites) | Each component in isolation: every accepted and rejected price and message form, line reassembly across arbitrary read sizes, hash-index collisions and deletion, exact error text, flush timing and exit codes |
| Scenario tests | The brief's worked example step by step, plus price and time priority, sweeps, price improvement, id reuse, and extreme prices and quantities |
| Differential tests | The engine vs. a deliberately naive reference engine on 10^6 random requests × 7 workload profiles × 2 seeds. Event streams and rejections must match exactly; book invariants are checked after every request, and quantity conservation at the end |
| Allocation test | Zero heap allocations over 10^6 requests through the whole pipeline; injected allocation failure is rejected cleanly |
| Golden end-to-end | The real binary on 9 hand-verified datasets in `data/golden/` (including the brief's example as pasted, with its comments and zero-width spaces, and a hostile input of NUL bytes, 200 KB lines and terminal escapes), comparing stdout, stderr and exit code byte for byte |
| Fuzzing | libFuzzer + ASan + UBSan over the whole pipeline, with invariants checked after every request |
| Stress suite (`scripts/stress.sh`) | The real binaries under load: 10^7-request runs byte-identical across GCC, Clang and a rerun; a 5×10^6-order book with 10^5 levels per side cancelled and swept to empty; a 3×10^7-request soak with flat memory; a 1 GiB single line (handled in ~3 MiB), 10^7 junk, blank and unknown-cancel lines; and output intact under a slow consumer |
| Sanitizers, coverage, static analysis | The whole suite under ASan+UBSan on GCC and Clang. Coverage ≥ 95% lines / ≥ 90% branches (100% of parser branches). clang-tidy and `-Werror` with strict warnings |

Generate larger datasets with the seeded generator; the output is identical on every platform:

```sh
build/release/gen_orders --profile tight --count 1000000 --seed 1 > /tmp/tight.csv
time build/release/matcher < /tmp/tight.csv > /dev/null 2>&1
```

## Packaging

`scripts/package.sh` writes `dist/order-matcher.zip` from the committed tree: all source, project, test, dataset and documentation files, with no build output.
