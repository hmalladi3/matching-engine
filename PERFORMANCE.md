# Performance

<!-- @spec DLV-PERF-003 -->

This document covers how fast the three request paths the assignment asks about are, which paths the design favors and why, what was measured and optimized, the trade-offs, and what would change for production. The numbers come from `bench/matcher_bench` and `scripts/stress.sh`. [How to reproduce](#reproducing) is at the end.

## Headline numbers

Three platforms, all built `-O3`:
- **x86:** GitHub Actions runners, 2 vCPUs, Ubuntu 24.04, Clang 18, timed with `rdtscp`. GitHub assigns the CPU model per job; the latencies below are from an AMD EPYC 9V45 (Zen 5), and throughput is given for every model the runs landed on.
- **Arm Linux:** Ubuntu 24.04 in Docker's VM on an Apple M4, Clang 18.
- **macOS:** the same M4, native, Apple Clang 17.

Latencies are mean ns per complete request with 10^5 resting orders.

| | x86 (EPYC 9V45) | Arm Linux (M4 VM) | macOS (M4 native) |
|---|---|---|---|
| **Real binary, 3×10^7 messages from a file** | **10.0–14.6M msg/s** ² | **16.0–17.2M msg/s** | **16.4–17.6M msg/s** |
| Add that checks for a match and rests | 33.5 ns ¹ | 16.5 ns | 9.2 ns |
| Add that fully fills one resting order | 37.2 ns ¹ | 31 ns | 12.5 ns |
| Cancel (order at the best level) | 33.4 ns ¹ | 15.8 ns | 8.2 ns |
| Cancel that empties a level deep in the book | 28.8 ns ¹ | 16.0 ns | 6.9 ns |
| Parse one request line | 11–13 ns | 9–10.5 ns | 9–10.4 ns |

¹ Includes one `rdtscp` timer pair (~20–25 ns); on the Arm platforms the per-request means come from coarser timers with lower read cost. The x86 percentiles are the precise tail figures: see [x86 results](#x86-results-github-actions).

² Depends on the runner's CPU generation: 10.0–10.3M on an EPYC 7763 (Zen 3), 11.7–12.0M on an EPYC 9V74 (Zen 4), 14.3–14.6M on an EPYC 9V45 (Zen 5). See [throughput by runner CPU](#throughput-by-runner-cpu).

Every one of these paths makes **zero heap allocations and zero system calls** in the steady state (a test enforces this). Output is byte-identical between the GCC and Clang builds across 3×10^7-message runs.

## The three paths the assignment asks about

L is the number of price levels on a side and d the distance in levels from the affected level to the best price.

| Path | Cost | Measured (Linux, 10^5 orders / 1,000 levels) |
|---|---|---|
| **Does an AddOrderRequest match?** | **O(1)**: compare its limit with `back()` of the opposite side's level vector | Part of every add: **16.5 ns** for an add that checks and then rests |
| **Removing a filled order** | **O(1)**: unlink the oldest node of the best level, erase it from the id index, recycle the node. If the level empties, pop it (and any retained empty levels behind it) | **31 ns** for an add that fully fills one order; **~7–12 ns per order** inside a multi-level sweep |
| **Removing a cancelled order** | **O(1)** to find (hash) and unlink it. If that empties its level: **O(1)** to retain the empty level for reuse (up to a cap), otherwise O(log L + d) to erase it | **14–16 ns** at the best or deepest level, including emptying it; 43 ns for a random order; 16 ns to empty the deepest of 1,000 levels |

## Design, and why each path is cheap

```
OrderBook
 ├─ bids : LevelStore<Buy>   std::vector<Level>, ascending  → best bid = back()
 ├─ asks : LevelStore<Sell>  std::vector<Level>, descending → best ask = back()
 ├─ pool : NodePool          2 MiB-aligned, hugepage-advised node array; free list; 32-bit indices
 └─ index: OrderIndex        open addressing, cache-line-blocked hash, load ≤ 1/2, hugepage-advised
Level = {price, sentinel}               16 bytes: four levels per cache line
Node  = {id, qty, price, next, prev}    32 bytes: two nodes per cache line
```

**Match detection.** The best opposite price is always the last element of a contiguous vector, and that level always has resting orders. The check is one load and one integer compare: prices are fixed-point `int64` with 8 decimal places, so the compare is exact and a single instruction. There is no floating point anywhere in the engine.

**Filled-order removal.** Each level keeps its orders in a circular doubly-linked FIFO threaded through the pool nodes, with a sentinel node, the same pattern as the Linux kernel's `list_head`. The oldest order is `sentinel.next`; removing it touches its two neighbours, one index slot and the pool's free-list head. A sweep through k resting orders costs O(k), with constant work per fill.

**Cancel.** The id index maps an order id to its node in expected O(1). Four consecutive ids share one 64-byte block of slots, so sequentially assigned ids stay cache-friendly. Because the FIFO is circular through a sentinel, **unlinking an order never needs to know which level it is in**. When a cancel empties a level behind the best, the empty level is **kept in place** (up to 256 per side) and reused when orders return to that price; only past that cap is it erased with a `memmove` of the better levels.

**Memory.** At startup the pool and index are sized for `--reserve N` orders (default 2^20, about 64 MB) and every page is touched, so no page faults happen while trading. On Linux both arrays are 2 MiB-aligned and ask for transparent hugepages before that first touch. Freed nodes and slots are reused most-recently-freed first, while still in cache. Capacity is checked **before** any mutation, so a request is always applied completely or rejected with no effect, even if growth fails.

**I/O.** Input is read with `read(2)` into a 1 MiB buffer and split with one `memchr` per line. A single-pass parser handles the forms nearly every line takes (`0,id,side,qty,price` and `1,id`) without trimming or searching; anything else, including every error, goes to the general parser, which is the only authority on diagnostics (a test checks the two agree on 500,000+ inputs, including random byte mutations). Output is formatted with `std::to_chars` directly into a 64 KiB buffer. Output is flushed right before each `read(2)`, i.e. only when the program might block: a large file costs one `write` per ~1 MiB of input, while an interactive user still sees each result immediately.

## Which paths are favored, and why

The design deliberately **favors everything near the top of the book and prices that recur**, because that is where real order flow goes: new orders cluster near the best prices, cancels are mostly of recent orders near the touch, every trade happens at the best price, and orders keep returning to the same few prices.

- **Top-of-book operations are O(1)** and touch only the end of the level vector, which is almost always in L1 cache.
- **Churn at an existing price anywhere in the book is O(1) or O(log L).** Emptying a deep level costs 16 ns and re-adding there costs 39–77 ns (1,000 to 100,000 levels), because the level is retained rather than erased and re-inserted.
- **Creating a level at a price never seen before, deep in the book, costs O(d)** data movement: 265 ns at 1,000 levels, 2 µs at 10,000 and 25 µs at 100,000. This is the one path the sorted-vector design pays for. The production fix is a dense tick ladder (below).

The alternative would be `std::map`, which makes every level operation O(log L) but puts a heap node and a pointer chase behind each one, *including* the common top-of-book case. Paying a rare, bounded `memmove` for new deep levels in exchange for cache-friendly O(1) at the top is the standard trade in low-latency book design.

## Results

**Setup.**
- **Hardware:** Apple M4 (10 cores). Linux numbers are from Ubuntu 24.04 in Docker's VM; macOS numbers are native.
- **Compilers:** Clang 18.1.3 and GCC 13.3.0 (Linux) and Apple Clang 17 (macOS), all `-O3 -DNDEBUG`.
- **Method:** 5 runs; each statistic is the median across runs. The real-binary figures are the best of 3 runs over 3×10^7 generated messages.
- **Workload:** the generated flows are deliberately hostile. 18% (mixed) to 34% (tight) of their lines are rejected, almost all cancels of orders that had already filled, because the generator cannot see fills. Each rejection costs a formatted diagnostic, so the throughput figures are conservative for realistic, mostly valid flows.
- **Timer:** the Arm generic timer ticks every 41.7 ns (both platforms), so individual latencies below ~40 ns show as 0 or 42 in the percentile columns. **The mean column is the precise figure** (computed from batch totals). Each sample also includes one timer read (~20–30 ns in the VM).
- **x86:** see [x86 results](#x86-results-github-actions). That run uses `rdtscp` (~10 ns resolution) and gives the precise percentile tails; `.github/workflows/x86-benchmark.yml` reproduces it.

### Per-request latency by book size (Linux, mean ns; p99 in parentheses)

A book of N resting orders spans min(N/20, 1000) levels per side.

| Scenario | 10^3 | 10^5 | 10^6 | | 10^5 GCC | 10^6 GCC |
|---|---|---|---|---|---|---|
| add, rests at best (no match) | 16.6 (42) | 16.5 (42) | 40.8 (167) | | 23.7 (125) | 46.3 (167) |
| add, creates new best level | 19.4 (42) | 19.4 (42) | 44.9 (208) | | 24.3 (42) | 49.9 (167) |
| add, at the deepest level's price | 28.1 (42) | 38.9 (83) | 59.3 (167) | | 27.1 (125) | 49.8 (167) |
| add, fully fills one resting order | 21.0 (42) | 31.0 (125) | 51.9 (167) | | 26.9 (83) | 50.9 (167) |
| add, sweeps 1 level | 73 (125) | 522 (708) | 5,824 (7,500) | | 512 (667) | 5,357 (7,000) |
| add, sweeps 10 levels | 711 (875) | 5,098 (6,292) | 60,809 | | 4,942 (6,167) | 62,184 |
| cancel, order at best level | 13.0 (42) | 15.8 (42) | 28.3 (42) | | 21.5 (42) | 34.6 (42) |
| cancel, order at deepest level | 12.0 (42) | 14.3 (42) | 28.0 (42) | | 21.3 (42) | 35.0 (83) |
| cancel, order at random level | 15.8 (42) | 42.7 (208) | 289 (458) | | 41.1 (125) | 286 (458) |
| cancel, empties best level | 14.1 (42) | 17.3 (42) | 17.4 (42) | | 23.2 (42) | 23.3 (42) |
| cancel, empties deepest level | 13.0 (42) | 16.0 (42) | 15.8 (42) | | 21.0 (42) | 21.3 (42) |

Sweep cost is proportional to the orders filled (1 level holds 10, 50 and 500 orders at the three sizes): **~7–12 ns per resting order filled** throughout.

### Level churn deep in the book (Linux, Clang, mean ns)

| Levels per side | 100 | 1,000 | 10,000 | 100,000 |
|---|---|---|---|---|
| add at a recurring deepest price (level reused) | 30 | 37 | 53 | 77 |
| cancel emptying the deepest level (level retained) | 12 | 13 | 14 | 16 |
| add at a **never-seen** deepest price (new level, O(L)) | 111 | 265 | 2,047 | 25,166 |
| cancel of such a level once the retention cap is full (erase, O(L)) | 100 | 250 | 1,994 | 24,562 |

### Latency on generated order flow (engine only, per request)

| Profile | p50 | p99 | p99.9 | mean (Clang / GCC) |
|---|---|---|---|---|
| tight (narrow band, heavy matching) | 42 | 125 | 208 | 38 / 39 |
| mixed (all behaviors, mid through zero) | 42 | 167 | 333 | 47 / 49 |
| sweep (large aggressive orders) | 42 | 167 | 667 | 40 / 42 |

These include one timer read per request; batch timing (below) puts the same engine work at 23–31 ns per request.

### Throughput by stage (batch timing, 5×10^6 messages, ns/msg)

| Stage | Clang 18 (Linux) | GCC 13 (Linux) | Apple Clang (macOS) |
|---|---|---|---|
| parse only | 9.1–10.5 | 9.1–10.2 | 8.8–10.4 |
| engine only (pre-parsed) | 23.9–31.0 | 26.9–35.9 | 22.7–29.7 |
| format only (1 trade + 2 fills) | 21.2 | 28 ¹ | — |
| **end to end (`run_app`, in memory)** | **58.5–59.4 = 16.8–17.1M msg/s** | **63.6–65.1 = 15.4–15.7M msg/s** | **56.0–57.2 = 17.5–17.9M msg/s** |
| **real binary, file → /dev/null** | **58–63 = 16.0–17.2M msg/s** | **62–70 = 14.4–16.0M msg/s** | **57–61 = 16.4–17.6M msg/s** |

¹ One full run showed 61 ns for this row; three reruns all gave 27.8 ns, so the outlier was a transient in the VM.

**Compiler choice depends on the platform.** On Arm, Clang is 7–10% faster end to end than GCC, mostly in output formatting. On x86 (EPYC 9V45), the two are within a few percent, and GCC is slightly faster on the mixed flow (64 vs 71 ns/msg in memory). Both are supported, and `.github/workflows/x86-benchmark.yml` measures both, so the choice for a deployment should come from running it on that hardware.

### Stress suite (real binaries; `scripts/stress.sh`)

| Test | Result |
|---|---|
| S1: 10^7 requests × 3 profiles | 13.1–14.3M msg/s (file in, file out); 70 MiB peak; stdout and stderr **byte-identical** across the Clang build, the GCC build and a rerun |
| S2: 5×10^6 orders over 10^5 levels per side; cancel half; sweep both sides | Exactly the expected 2.6M trades, book empty afterwards, 3.5 s, 649 MiB peak |
| S3: 3×10^7-request soak | Memory flat at 68 MiB from 25% of the run to the end |
| S4: a 1 GiB single line | One diagnostic, trailing trade correct, **3 MiB peak**: the line is streamed, never buffered |
| S4: 10^7 blank/comment, garbage, and unknown-cancel lines | Exactly one diagnostic per bad line; 0.53–0.56 s per 10^7 diagnostics; 3 MiB peak |
| S5: stdout through a throttled reader | 9 MB of output byte-identical to the unthrottled run |

### x86 results (GitHub Actions)

The latency tables come from a 2-vCPU AMD EPYC 9V45 (Zen 5) cloud runner, Ubuntu 24.04, pinned to one core with `taskset`, timed with `rdtscp` + `lfence`. Every sample includes one timer pair (~20–25 ns), and the percentiles resolve to ~10 ns. The full output is produced by `.github/workflows/x86-benchmark.yml`.

**Per-request latency, Clang 18 (ns)**

| Scenario | 10^3 orders p50 / p99 / p99.9 | 10^5 orders p50 / p99 / p99.9 | 10^6 orders p50 / p99 / p99.9 |
|---|---|---|---|
| add, rests at best | 30 / 50 / 70 | 30 / 40 / 60 | 40 / 280 / 361 |
| add, fully fills one order | 40 / 50 / 60 | 40 / 50 / 50 | 40 / 290 / 361 |
| cancel, order at best level | 30 / 30 / 40 | 30 / 40 / 40 | 30 / 40 / 270 |
| cancel, order at random level | 30 / 40 / 40 | 50 / 60 / 170 | 260 / 481 / 611 |
| cancel, empties deepest level | 30 / 40 / 40 | 30 / 40 / 50 | 30 / 60 / 200 |

**Tails are tight:** up to 10^5 resting orders, p99.9 stays within 2× of p50 for every book operation. At 10^6 the p99 of adds rises to ~280 ns, which is the DRAM miss described below.

**Generated order flow, engine only (ns per request)**

| Profile | p50 | p99 | p99.9 | mean (Clang / GCC) |
|---|---|---|---|---|
| tight | 50 | 250 | 361 | 65 / 64 |
| mixed | 60 | 270 | 461 | 76 / 75 |
| sweep | 40 | 300 | 971 | 69 / 69 |

**Deep-level churn (mean ns, Clang):** re-adding at a recurring deep price costs 37–73 ns and a cancel emptying a deep level 30 ns, at every book depth from 100 to 100,000 levels. A new level at a never-seen deep price costs 197 ns at 1,000 levels and 11.9 µs at 100,000 (O(L), as designed).

**Throughput (ns/msg)**

| Stage | Clang 18 | GCC 13 |
|---|---|---|
| parse only | 11.1–13.4 | 10.8–12.5 |
| engine only (pre-parsed) | 41.4–48.2 | 38.6–42.5 |
| format only (1 trade + 2 fills) | 25.1–26.8 | 30.6–31.6 |
| end to end (`run_app`, in memory) | 71.3–73.6 | 64.2–70.6 |
| **real binary, file → /dev/null** | **68.6–70.0 = 14.3–14.6M msg/s** | **71.1–72.6 = 13.8–14.1M msg/s** |

A shared cloud vCPU is slower per core than the M4 (the engine alone is ~40 vs ~25 ns per request); a dedicated, isolated server core would be faster than these figures.

#### Throughput by runner CPU

The real binary on the same 3×10^7-message inputs (file → /dev/null, pinned with `taskset`), one row per runner CPU the benchmark workflow landed on:

| Runner CPU | Runs | Clang 18 | GCC 13 |
|---|---|---|---|
| AMD EPYC 9V45 (Zen 5) | 1 ³ | 14.3–14.6M msg/s | 13.8–14.1M msg/s |
| AMD EPYC 9V74 (Zen 4) | 1 | 11.7–12.0M msg/s | 11.4–11.5M msg/s |
| AMD EPYC 7763 (Zen 3) | 3 | 10.0–10.3M msg/s | 9.3–9.5M msg/s |

The spread is the hardware: the same commit on the 9V74 measured within 1% of the A/B runs on that model. Clang is 3–7% faster than GCC on every model.

³ Measured before pass 3's kept change (the inline capacity check, 3–4% faster in the A/B), so the final code would be at least as fast; the other rows are the final code.

### Same hardware without the VM (macOS native, Apple Clang 17, mean ns)

| | 10^5 orders | 10^6 orders |
|---|---|---|
| add, rests at best | 9.2 | 26.6 |
| add, at the deepest level's price | 14.9 | 31.5 |
| add, fully fills one order | 12.5 | 33.5 |
| cancel, order at best level | 8.2 | 10.1 |
| cancel, order at random level | 9.2 | 107.9 |
| cancel, empties deepest level | 6.9 | 7.2 |

Natively, CPU-bound operations run about twice as fast as in the VM, and touching a random order among 10^6 costs ~108 ns instead of ~289 ns. Inside a VM every TLB miss needs a two-stage page-table walk; the gap is evidence that the large-book cost is address translation and cache misses, not algorithmic work.

## How the numbers got here

The first complete version was correct and cleanly O(1) on the three paths, but profiling showed where real time went. Two broad optimization passes followed, then an x86 loop of profile → candidates → A/B rounds that ran until a round found nothing worth keeping. Every change kept all tests green, including the differential test against the naive reference engine.

| | Real binary / S1 (Linux) | `run_app` end to end (Linux) | Resting add at 10^6 orders |
|---|---|---|---|
| First complete version | 7.7–8.5M msg/s | 104 ns/msg | 123 ns |
| Pass 1: cache-line-blocked index hash, hugepage-backed arrays | 9.5–10.0M msg/s | 90 ns/msg | 40 ns |
| Pass 2: single-pass parser, flush inside `read()`, retained levels, in-place diagnostics | **13.1–14.3M msg/s** (16.2–16.4M to /dev/null) | **59 ns/msg** | 41 ns |

**Pass 1: the large-book case.** At 10^6 resting orders the pool and index (~64 MB) exceed the last-level cache, and every fresh id hashed to a random index slot: a DRAM miss plus TLB misses.
- **Index hash:** 4 consecutive ids now share one 64-byte cache line, with blocks placed by Fibonacci hashing after `x ^= x >> 12`. Sequential ids never collide on blocks, and every power-of-two stride from 2^2 to 2^48 still spreads well (a test enforces this; plain Fibonacci hashing fails it).
- **Hugepages:** the pool and index are 2 MiB-aligned and advised `MADV_HUGEPAGE` before first touch. That is what gets them 2 MB pages under the `madvise` policy most Linux servers ship with (verified in a test through `/proc/self/smaps`). Under that policy: resting add at 10^6 orders 138 → 39 ns, fill 131 → 50 ns, random cancel 322 → 274 ns.

**Pass 2: the whole pipeline.** A CPU profile of the real binary showed about half of all time going from bytes to a parsed request (about eight `memchr` calls per line, plus trimming), and a further 7% in writing diagnostics.
- **Single-pass parser** for clean lines, with the general parser kept as the only authority: parsing 36 → 10 ns per line.
- **Flush inside `read()`**: the same flush-before-blocking rule without a per-line scan.
- **Retained empty levels**: emptying the deepest of 1,000 levels 167 → 7 ns (macOS), re-adding there 184 → 15 ns.
- **Diagnostics formatted in place**: one buffer reservation instead of ~10 appends; 10^7 diagnostics 0.78 → 0.53 s.

**Pass 3: the x86 loop.** Passes 1–2 were measured on the development machine. Pass 3 accepted or rejected changes only on the target, x86-64 Linux (GitHub Actions runners). Each round works like this:
- **Profile:** `perf` on the real binary.
- **Candidates:** each on its own branch, with all tests green.
- **A/B:** every candidate and the baseline process the same 3×10^7-request inputs (the mixed and tight profiles) in 15–25 shuffled, interleaved rounds in one job.
- **A/A control:** the baseline also runs a second time under another name. Its spread is the noise floor.
- **Bar:** a change is kept only if it is ≥1% faster, clearly outside that spread, on both profiles, and with both GCC and Clang.

| Round | Candidate | Result (median change in ns/msg, mixed / tight) | Decision |
|---|---|---|---|
| 1 | Inline fast path for the capacity check (one branch instead of a call per add) | −3.0% / −3.0% (Clang, AMD EPYC 9V74); −4.3% / −4.0% (GCC, Intel Xeon 8573C) | **Kept** |
| 2 | Parse one buffered line ahead and prefetch its index slot | −3.6% / −3.4% (Clang, AMD EPYC 7763); +0.9% / +0.2% (Clang, Intel Xeon 8370C); −0.1% to −2.3% (GCC, within noise) | Rejected |

Round 2 found no keeper, so the loop stopped there. In the last profile, `OrderIndex::find` takes 18–26% of the time on both CPU vendors, parsing 12–16% and reject diagnostics 5–10%. The index lookup is almost entirely DRAM latency: the 32 MB index is larger than the runners' L2 cache. Prefetching the next request's slot hid that latency on one AMD part only. On Intel the lookahead cost as much as it saved. The remaining large gains need changes to the design, and those are on the production list: a binary protocol, direct indexing of venue ids, and the dense ladder.

**Tried and rejected** (measured, then not kept):

| Idea | Result |
|---|---|
| Identity hash (`id & mask`) | Fastest for sequential ids, **20–40× slower** for strided ids: a denial-of-service risk |
| 8- or 16-id hash blocks | Faster sequential adds, but random cancels up to 75% slower (longer probe clusters) |
| Multiply-fold-multiply block mixer | Fixes strides, but turns sequential ids into random placement: random cancels 2× slower |
| Single-probe cancel (find and erase the index entry in one pass) | No measurable change |
| SWAR parsing (8 digits per 64-bit multiply sequence) | x86: **3–12% slower** end to end on both compilers; the fixed cost exceeds the savings on 1–7-digit numbers |
| Branch-free printable check before escaping diagnostics | x86: −1.2% / −1.3% (Clang, AMD) but **+4.0% / +11.5%** (GCC, Intel) |
| Inline word-at-a-time newline search instead of `memchr` | x86: +0.6% to −0.8%, within noise (glibc's vectorized `memchr` is already strong) |
| Linear scan of the 8 levels nearest the best before binary search | x86: +0.1% / −1.7% (Clang), +0.3% / −1.3% (GCC); no gain on mixed flow, and the gain on tight flow is within the A/A range |
| Parse one line ahead and prefetch its index slot (with or without copying the lookahead request) | x86: −3.5% on one AMD part, +0.9% on Intel, noise with GCC; depends on the CPU, and adds lookahead logic to the input loop |

**Why only same-job comparisons count.** Measurements taken in separate sessions drifted by up to ~5%, as large as the effects being chased. The runners' CPU model also changes from job to job: the same capacity-check change measured −3% on one AMD part and −4% on an Intel part. Only interleaved rounds inside one job see identical conditions. Candidates chosen from development-machine numbers were rechecked on x86. That reversed the capacity-check decision: on the development machine it looked like −0.7% to −2.3%, below the bar.

**One regression to state plainly:** with 4-id index blocks, a book holding *every* id in a dense range has longer probe clusters, so each fill inside a sweep costs ~3 ns more at 10^5 orders (7 → 10 ns per order in that synthetic book). Realistic flows with churn do not produce such dense books, and every generated-flow profile got 35–40% faster.

## Trade-offs and their costs

| Decision | Benefit | Cost / risk | Mitigation |
|---|---|---|---|
| Sorted vector of levels | O(1) top of book, contiguous memory, no per-level allocation | O(d) `memmove` to create a level at a never-seen deep price | Retained levels make recurring prices cheap; production option is a dense tick ladder |
| Retained empty levels (cap 256 per side) | O(1) cancels that empty a level; no re-insert when orders return | Up to 256 pops when the best level empties with retained levels behind it; ~48 bytes per retained level | Cap measured (0/64/256/1024: gains at any nonzero cap, tails flat); at the node limit retained levels are released before any order is rejected |
| Circular sentinel FIFO in a node pool | O(1) cancel with no level search; no allocation | One extra pool node per level | Negligible |
| Preallocate and never shrink | No allocation, page faults or system calls while trading | Memory stays at its peak | Size `--reserve` for the peak day; engines restart each trading session |
| Doubling growth past the reservation | Never rejects valid input just because a default was too small | One-off O(n) copy/rehash spike (~4 ms at 10^6 orders; p99.9 stays at 250 ns) | `--reserve`; production options below |
| Capacity reserved *before* matching | Every request is all-or-nothing, even when allocation fails | An add that would have freed capacity by filling can be rejected at the hard node limit | Only reachable at 2^32 − 1 nodes or on allocation failure |
| Cache-line-blocked hash | Sequential ids share cache lines | Dense-range books cost ~3 ns more per sweep fill | Realistic flows 35–40% faster |
| Unseeded hash | Cheap, no sequential collisions, robust to power-of-two strides | Crafted ids could still force collisions | Production: a seeded hash |
| Fixed-point price, 8 decimals | Exact, one-instruction compares | Range ±9.2 × 10^10; finer prices rejected | Covers every real futures tick; out-of-range input is a clear error |
| Single-threaded | No locks or atomics; deterministic output | One core per book | Shard instruments across cores |

## What I would change for production

Roughly in order of expected impact for a real futures venue or trading system:

1. **Dense price ladder (stretch goal #1).** A futures contract has a known tick size and a bounded daily range, which allows an array indexed by `(price − base) / tick` plus a hierarchical bitmap of non-empty levels (`lzcnt`/`tzcnt` to find the next best). Every level operation becomes O(1), including the one path still O(L) here: a new level at a never-seen deep price. The `LevelStore` interface is the seam where it plugs in. It is not done here because the assignment allows any decimal price with no tick size.
2. **Order-id indexing for the real id scheme.** Venue-assigned ids are typically sequential per session, so they can index a slab directly (`id − session base`): no hashing and no DRAM miss even for random cancels at 10^6 orders, the costliest operation measured here. Where ids are client-chosen, use a **seeded** hash so crafted ids cannot force collisions.
3. **Binary protocol instead of CSV.** Parsing is now ~10 ns, but a fixed-layout binary format (SBE or ITCH-style) reduces it to a few loads and removes the line-splitting pass.
4. **Kernel-bypass networking and a busy-polling pinned thread.** Input comes from the network, not stdin: Solarflare/AMD `ef_vi` or Onload, or DPDK, with the matching thread pinned to an isolated core (`isolcpus`, `nohz_full`, IRQ affinity) and never sleeping.
5. **Pipeline with the LMAX Disruptor pattern (stretch goal #2).** Decoder → engine → encoder on separate cores over single-producer/single-consumer rings with cache-line-padded sequences. The engine thread then only matches. It costs one cross-core hop (~50–100 ns) per message, so it pays off when decoding and encoding cost as much as matching.
6. **Memory beyond transparent hugepages (stretch goal #3).** Reserve explicit hugetlbfs pages so a fragmented host cannot fall back to 4 KB pages; `mlock` the book; allocate on the matching core's NUMA node; back the pool with a large `mmap` reservation so growth never copies; rehash incrementally to remove the growth spike.
7. **Scale out by sharding instruments across cores**, each book single-threaded, with a symbol router in front. This is how exchanges scale; it needs no locking on a book.
8. **Build and tuning.** Profile-guided optimization and LTO; `-march` for the deployment CPU; verify hot-path code generation with `perf` and the disassembly; maintain per-level aggregate quantities for market-data publication.
9. **Operational requirements:** journaling of inputs for deterministic replay and recovery; pre-trade risk checks; self-trade prevention, which needs an account or trader id on each order so that two orders from the same owner cancel (the resting one, the aggressing one, or both, per venue policy) instead of trading; IOC/FOK/market orders; per-session sequence numbers.

## Reproducing

```sh
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=clang++   # or g++
cmake --build build/release -j
taskset -c 2 build/release/bench/matcher_bench            # Linux: pin to one core; ~5 min
build/release/bench/matcher_bench --quick                 # a few seconds
scripts/stress.sh                                          # stress suite (~5 min, a few GB of disk)

# Real-binary throughput on 3x10^7 messages
build/release/gen_orders --profile mixed --count 30000000 --seed 3 > /tmp/mixed.csv
time build/release/matcher < /tmp/mixed.csv > /dev/null 2>&1

# The Linux numbers above, from any host
docker build -t order-matcher .
docker run --rm order-matcher bash -c 'cmake -S . -B b -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_CXX_COMPILER=clang++-18 && cmake --build b --target matcher_bench && b/bench/matcher_bench'
```

The harness prints the CPU, compiler, flags, timer source and resolution, and seed with every run. Each per-operation scenario times a single engine call and then undoes it untimed, so the book keeps the same shape across samples. Each statistic is the median across 5 runs.
