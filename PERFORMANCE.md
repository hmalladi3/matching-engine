# Performance

<!-- @spec DLV-PERF-003 -->

This document covers how fast the three request paths the assignment asks about are, which paths the design favors and why, the trade-offs made, and what would change for production. The numbers come from `bench/matcher_bench`. [How to reproduce](#reproducing) is at the end.

## Summary

In the table below, L is the number of price levels on one side of the book and d is the distance in levels from the affected level to the best price. Measured values are the mean ns per complete request (Linux, Clang 18 `-O3`) on a book of 100,000 resting orders with 1,000 levels per side. Full tables for both compilers are in [Results](#results).

| Path | Cost | Measured |
|---|---|---|
| **Does an AddOrderRequest match?** | **O(1)**: compare its limit with `back()` of the opposite side's level vector | Part of every add: **~16 ns** for an add that checks for a match and then rests |
| **Removing a filled order** | **O(1)**: unlink the oldest node of the best level, erase it from the id index, return it to the pool; `pop_back` the level if it empties | **~24 ns** for an add that fully fills one resting order; **~10 ns per order** within a multi-level sweep |
| **Removing a cancelled order** | **O(1)** to find it (hash) and unlink it; **+O(log L + d)** only if the cancel empties its level | **~14–16 ns** (at the best or deepest level), ~34 ns for a random order; emptying the best level ~17 ns; emptying the deepest of 1,000 levels ~200 ns |

All three paths make **zero heap allocations** and **zero system calls** in the steady state; a dedicated test enforces this. End to end, the binary processes **about 11 million messages per second** on one core, including reading, parsing, matching and formatting. At 10^7 messages through the real binary it sustains 9.5–10M messages/s in 70 MiB of memory (stress test S1).

## Design, and why each path is cheap

```
OrderBook
 ├─ bids : LevelStore<Buy>   std::vector<Level>, ascending  → best bid = back()
 ├─ asks : LevelStore<Sell>  std::vector<Level>, descending → best ask = back()
 ├─ pool : NodePool          std::vector<Node> preallocated; free list; 32-bit indices
 └─ index: OrderIndex        open addressing, linear probing, cache-line-blocked hash, load ≤ 1/2
Level = {price, sentinel}   16 bytes: four levels per cache line
Node  = {id, qty, price, next, prev}   32 bytes: two nodes per cache line
```

**Match detection.** The best opposite price is always the last element of a contiguous vector, so the check is one load and one integer compare. Prices are fixed-point `int64` (8 decimal places), so the compare is exact and a single instruction. There is no floating point anywhere in the engine.

**Filled-order removal.** Each level keeps its orders in a circular doubly-linked FIFO threaded through the pool nodes, with a sentinel node, the same pattern as the Linux kernel's `list_head`. The oldest order is `sentinel.next`. Removing it touches its two neighbours, one index slot and the pool's free-list head. If the level empties, it is the vector's last element, so it is removed with `pop_back`. A sweep through k resting orders therefore costs O(k), with constant work per fill.

**Cancel.** The id index maps an order id to its pool node in expected O(1). Most lookups touch one cache line: the table is at most half full, linear probing keeps collisions adjacent, and 4 consecutive ids share one cache-line block, so sequentially assigned ids stay cache-friendly (see [the optimization pass](#optimization-pass-the-large-book-case)). Because the FIFO is circular through a sentinel, **unlinking an order never needs to know which level it belongs to**. Only a cancel that removes a level's *last* order has to locate the level: a binary search by price, then a `vector::erase`, which is a `memmove` of the d levels that are better than it. Deletion from the index uses backward shift instead of tombstones, so probe lengths do not degrade under heavy add/cancel churn.

**Memory.** At startup the pool and index are sized for `--reserve N` orders (default 2^20, about 64 MB) and every page is touched, so no page faults happen while trading. On Linux both arrays are 2 MiB-aligned and ask for transparent hugepages before that first touch. After that, freed nodes and slots are reused (most recently freed first, since that memory is still in cache). Nothing on the request path calls `malloc`. Capacity is checked **before** any mutation, so a request is always applied completely or rejected with no effect, even if growth fails.

**I/O.** Input is read with `read(2)` into a 1 MiB buffer and split with `memchr`. Fields are parsed in place from `string_view`s. Output is formatted with `std::to_chars` directly into a 64 KiB buffer. Output is flushed only when the reader is about to block: a file of millions of lines costs one `write` per ~1 MiB of input, while someone typing lines still sees each result immediately.

## Which paths are favored, and why

The design deliberately **favors everything near the top of the book**. That is where nearly all real order flow goes: new orders cluster at or near the best prices, cancels are mostly of recently placed orders near the touch, and every trade happens at the best price.

- **Operations at the best price are O(1) and touch only the end of the level vector**, which is almost always in L1 cache. That covers adding at or inside the best price, matching, filling, and emptying the best level.
- **Operations deep in the book pay O(d) data movement.** Creating or deleting a level d levels away from the best price moves d × 16 bytes. The benchmark's worst case, a new level behind every existing one, costs ~200 ns at 1,000 levels, ~1.7 µs at 10,000 and ~24 µs at 100,000: linear, as designed. Adding to or cancelling from an *existing* deep level stays O(1) / O(log L).

The alternative would be `std::map`, which makes every level operation O(log L) but puts a heap node and a pointer chase behind each one, *including* the common top-of-book case. Paying a rare, bounded `memmove` for deep-book level changes in exchange for cache-friendly O(1) at the top is the standard trade in low-latency book design.

The second deliberate asymmetry is **cancel vs. add at a new deep level**. Both can pay O(d), but only when a level is created or destroyed. Cancelling or filling an order in a level that still has other orders never searches.

## Trade-offs and their costs

| Decision | Benefit | Cost / risk | Mitigation |
|---|---|---|---|
| Sorted vector of levels | O(1) top of book, contiguous memory, no per-level allocation | O(d) `memmove` for deep level insert/erase; O(L) worst case | Measured and bounded; production option is a dense tick ladder (below) |
| Circular sentinel FIFO in a node pool | O(1) cancel with no level search; no allocation | One extra pool node per level | Negligible: 32 bytes per level |
| Preallocate and never shrink | No allocation, page faults or system calls while trading | Memory stays at its peak | Size `--reserve` for the peak day; engines restart each trading session |
| Doubling growth past the reservation | Never rejects valid input just because a default was too small | One-off O(n) copy/rehash spike: the slowest single add while growing to 10^6 orders took ~4 ms (p99.9 stays at ~210 ns) | `--reserve`; production options below |
| Capacity reserved *before* matching | Every request is all-or-nothing, even when allocation fails | An add that would have freed capacity by filling can be rejected when the book is at its hard limit | Only reachable at the node limit (2^32 − 1) or on allocation failure |
| Cache-line-blocked hash (4 ids per block) | Sequential ids share cache lines: large-book adds ~3× faster | Books holding *every* id in a dense range form longer probe clusters, so each fill in a sweep costs ~3 ns more at 10^5 orders | Realistic flows with churn are 35–40% faster overall (measured below) |
| Unseeded hash | One multiply, no collisions for sequential ids, robust to every power-of-two stride | Crafted ids could still force collisions (a denial-of-service vector) | Production: a seeded hash (below) |
| Fixed-point price, 8 decimals | Exact, one-instruction compares; no floating-point surprises | Range ±9.2 × 10^10; finer prices rejected | Covers every real futures tick; out-of-range input is a clear error, never rounded |
| Single-threaded | No locks or atomics on the hot path; deterministic output | One core per book | Shard instruments across cores (below) |

## Results

**Setup.**
- **Hardware:** Apple M4 (10 cores) running Ubuntu 24.04 in Docker's Linux VM, which is the reference Linux environment for this submission (`docker build`, then the commands in [Reproducing](#reproducing)).
- **Compilers:** Clang 18.1.3 and GCC 13.3.0, both `-O3 -DNDEBUG`.
- **Method:** 5 runs; each statistic is the median across runs.
- **Timer:** the Arm generic timer (`cntvct_el0`), which ticks every 41.7 ns in this VM. Individual latencies below ~40 ns therefore appear as 0 or 42, and **the mean column (computed from batch totals) is the precise figure**. Each sample also includes one timer read (~20–30 ns in the VM).
- **Caveat:** no x86 server was available. On an isolated x86 core with `rdtscp`, which the harness uses automatically, the per-operation percentiles would resolve to single nanoseconds.

### Per-request latency by book size (mean ns; p99 in parentheses)

A book of N resting orders spans min(N/20, 1000) levels per side, with ~10 orders per level per side until the level cap.

| Scenario | 10^3 orders | 10^5 orders | 10^6 orders | | 10^5, GCC 13 | 10^6, GCC 13 |
|---|---|---|---|---|---|---|
| add, rests at best (no match) | 17.0 (42) | 16.2 (42) | 40.4 (167) | | 21.0 (42) | 45.7 (167) |
| add, creates new best level | 18.5 (42) | 18.6 (42) | 43.0 (167) | | 23.9 (42) | 49.1 (167) |
| add, creates level behind all others | 42.7 (83) | 221 (292) | 241 (417) | | 205 (250) | 230 (417) |
| add, fully fills one resting order | 20.9 (42) | 23.5 (42) | 50.1 (167) | | 25.9 (42) | 49.6 (167) |
| add, sweeps 1 level | 73.7 (125) | 512 (625) | 5,391 (6,500) | | 518 (625) | 5,240 (6,333) |
| add, sweeps 10 levels | 698 (875) | 5,049 (6,333) | 58,455 | | 5,102 (6,375) | 58,678 |
| cancel, order at best level | 12.8 (42) | 15.6 (42) | 29.6 (42) | | 21.0 (42) | 31.6 (42) |
| cancel, order at deepest level | 11.9 (42) | 14.3 (42) | 28.9 (42) | | 20.3 (42) | 34.7 (83) |
| cancel, order at random level | 15.7 (42) | 33.6 (83) | 260 (417) | | 39.1 (83) | 257 (417) |
| cancel, empties best level | 14.1 (42) | 16.8 (42) | 16.9 (42) | | 23.1 (42) | 23.3 (42) |
| cancel, empties deepest level | 35.1 (42) | 204 (250) | 204 (250) | | 197 (208) | 197 (208) |

Sweep cost is proportional to the orders filled: 1 level holds 10, 50 and 500 orders at the three sizes, so a fill costs **~7–11 ns per resting order** throughout.

### Deep-book worst case: level created and removed behind every other level

| Levels per side | 100 | 1,000 | 10,000 | 100,000 |
|---|---|---|---|---|
| add (mean ns, Clang / GCC) | 58 / 47 | 217 / 201 | 1,954 / 1,943 | 25,024 / 25,028 |
| cancel (mean ns, Clang / GCC) | 50 / 41 | 206 / 190 | 1,937 / 1,923 | 24,703 / 24,878 |

This is linear in L, exactly the O(d) `memmove` the design accepts. It stays under 250 ns for books up to ~1,000 levels, which covers typical futures books.

### Latency on generated order flow (engine only, per request)

| Profile | p50 | p99 | p99.9 | mean (Clang / GCC) |
|---|---|---|---|---|
| tight (narrow band, heavy matching) | 42 | 125 | 208 | 35 / 38 |
| mixed (all behaviors, mid through zero) | 42 | 125 | 292 | 46 / 49 |
| sweep (large aggressive orders) | 42 | 125–167 | 667 | 37 / 40 |

These include one timer read per request. In batch timing (below) the same engine work costs 22–31 ns per request.

### Throughput (batch timing, 5×10^6 messages)

| Stage | Clang 18 | GCC 13 |
|---|---|---|
| parse only | 34–36 ns/msg | 36–37 ns/msg |
| engine only (pre-parsed) | 22–31 ns/msg | 24–35 ns/msg |
| format only (1 trade + 2 fills) | 22 ns | 29 ns |
| **end to end (`run_app`, in memory)** | **89–90 ns/msg = 11.1–11.2M msg/s** | **100–101 ns/msg = 9.9–10.1M msg/s** |

**Compiler choice:** Clang is **about 11% faster end to end**, mostly in output formatting and in the engine on the mixed profile. GCC is marginally faster on a few deep-level operations. The recommended build is therefore Clang, based on these measurements.

### Stress suite (real binaries; `scripts/stress.sh`)

| Test | Result |
|---|---|
| S1: 10^7 requests × 3 profiles | 9.5–10.0M msg/s (file in, file out), 70 MiB peak; stdout and stderr **byte-identical** across Clang, GCC and a rerun |
| S2: 5×10^6 orders, 10^5 levels per side; cancel half; sweep both sides | Exactly the expected 2.6M trades, book empty afterwards, 3.7 s, 649 MiB peak (pool and index grew from the 2^20 default) |
| S3: 3×10^7-request soak | Memory flat at 68 MiB from 25% of the run to the end |
| S4: a 1 GiB single line | One diagnostic, trailing trade correct, **3 MiB peak**: the line is streamed and never buffered |
| S4: 10^7 blank, comment, garbage and unknown-cancel lines | Exactly one diagnostic per bad line, 0.8–0.9 s per 10^7 lines, 3 MiB peak |
| S5: stdout through a throttled reader | 9 MB of output byte-identical to the unthrottled run |

### Same hardware without the VM (macOS 15, Apple Clang 17, native)

| Mean ns per request | 10^5 orders | 10^6 orders |
|---|---|---|
| add, rests at best | 9.2 | 27.7 |
| add, fully fills one order | 12.0 | 32.3 |
| cancel, order at best level | 7.6 | 9.6 |
| cancel, order at random level | 10.4 | 104.7 |
| cancel, empties deepest level | 163 | 165 |
| end to end (`run_app`, in memory) | 93–96 ns/msg = 10.5–10.7M msg/s | |

Natively, the pure-CPU operations run about **twice as fast** as in the Linux VM, and the cost of touching a random order among 10^6 is ~105 ns instead of ~260 ns. Inside a VM every TLB miss needs a two-stage page-table walk, so the gap is itself evidence that the large-book cost is address translation and cache misses, not algorithmic work. It is also why hugepages are near the top of the production list. End-to-end throughput is similar on both, since it is dominated by parsing and formatting, which are CPU-bound.

### Optimization pass: the large-book case

The first measurements showed that at 10^6 resting orders, adds and fills cost ~120 ns versus ~20 ns at 10^5. The cause was memory, not the algorithm: the pool and index (~64 MB) exceed the last-level cache, and every fresh order id hashed to a random index slot, costing a DRAM miss plus TLB misses. Two changes, each chosen from measurements (details and rejected alternatives in `docs/llds/order-book.md`):

1. **Cache-line-blocked index hash.** 4 consecutive ids share one 64-byte block of slots, and blocks are placed by Fibonacci hashing after `x ^= x >> 12`. Sequentially assigned ids, which is how exchanges number orders, now fill a cache line four at a time, and never collide on blocks. Every power-of-two stride from 2^2 to 2^48 still spreads well; a test enforces this, and plain Fibonacci hashing fails it.
   - **Rejected:** identity hashing (20–40× slower on strided ids, a denial-of-service risk), 8- and 16-id blocks (random cancels up to 75% slower), and a multiply-fold-multiply mixer (turns sequential ids into random placement: random cancels 2× slower).
2. **Hugepage-backed arrays.** The pool and index are 2 MiB-aligned and advised `MADV_HUGEPAGE` before first touch. Under the `madvise` transparent-hugepage policy most Linux servers ship with, this is what gets them 2 MB pages; a Linux test reads `/proc/self/smaps` to verify.

| At 10^6 resting orders (Linux, Clang 18, mean ns) | Before | After |
|---|---|---|
| add, rests at best (THP policy `madvise`) | 138 | **39** |
| add, fully fills one order (THP `madvise`) | 131 | **50** |
| cancel, random order (THP `madvise`) | 322 | **274** |
| add, rests at best (THP `always`) | 123 | **40** |
| generated order flow, per request (tight / mixed / sweep) | 58 / 68 / 61 | **35 / 46 / 37** |
| end to end | 104 ns (9.6M msg/s) | **90 ns (11.1M msg/s)** |
| ⚠️ sweep fill, dense 10^5-order book (per order filled) | 7.3 | 10.1 |

**The regression, stated plainly:** the benchmark's synthetic books hold *every* id in a dense range, so full 4-slot blocks sit next to each other and probe clusters get longer. Each erase during a sweep then scans a little further, about +3 ns per filled order at 10^5 orders. Realistic flows with churn do not produce such dense books, and every generated-flow profile improved by 35–40%.

### What the numbers say about the design

- **Top-of-book operations are flat while the book fits in cache.** From 10^3 to 10^5 resting orders, adds, fills and cancels cost 12–24 ns.
- **At 10^6 orders, sequential-id adds and fills and top-of-book cancels cost 30–50 ns** in the VM (10–32 ns natively), within ~2.5× of the small-book cost.
- **Touching a truly random order among 10^6 costs ~260 ns in the VM and ~105 ns natively.** That is one or two DRAM misses for the index slot and the order's node, plus its FIFO neighbours. No in-memory layout makes random access across 64 MB as fast as a cache hit. The production remedies are direct indexing of venue-assigned ids and prefetching (below).
- **The O(d) deep-level cost behaves exactly as predicted,** and parsing, not matching, is the largest share of end-to-end time (~35 of ~90 ns).

## What I would change for production

Roughly in order of expected impact for a real futures venue or trading system:

1. **Dense price ladder (stretch goal #1).** A futures contract has a known tick size and a bounded daily price range. That allows an array indexed by `(price − base) / tick`, a hierarchical bitmap of non-empty levels, and `lzcnt`/`tzcnt` to find the next best level. Every level operation becomes O(1), including the deep-book cases that cost O(d) here. The `LevelStore` interface is the seam where it plugs in; the rest of the book is unchanged. I did not do it here because the assignment allows any decimal price with no tick size.
2. **Binary protocol instead of CSV.** Parsing is the largest single cost in the pipeline (~35 ns of ~90 ns per message). A fixed-layout binary format (SBE or ITCH-style) turns parsing into a few loads.
3. **Kernel-bypass networking and a busy-polling pinned thread.** In production, input comes from the network, not stdin. The standard stack is Solarflare/AMD `ef_vi` or Onload, or DPDK, with the matching thread pinned to an isolated core (`isolcpus`, `nohz_full`, IRQ affinity) busy-polling its queue so it never sleeps.
4. **Pipeline with the LMAX Disruptor pattern (stretch goal #2).** Reader/decoder → engine → encoder/publisher on separate cores, connected by single-producer/single-consumer ring buffers with cache-line-padded sequence counters. The engine thread then does only matching. This raises throughput at the cost of one cross-core hop (~50–100 ns) per message, so it is worth it only when decoding and encoding cost as much as matching, which the numbers above suggest they do.
5. **Memory (stretch goal #3).**
   - Back the pool with a large virtual-address reservation (`mmap` with `MAP_NORESERVE`), so growth never copies or moves nodes.
   - Go beyond transparent hugepages (already requested): reserve explicit hugetlbfs pages so a fragmented host cannot fall back to 4 KB pages, and `mlock` the book to keep it resident.
   - Allocate on the NUMA node of the matching core.
   - Rehash incrementally (as Redis does) to remove the index's growth spike.
6. **Order-id indexing for the real id scheme.** Venue-assigned ids are typically sequential per session, so they can index a slab directly (id − session base): no hashing, perfect locality, and no DRAM miss even for random cancels at 10^6 orders. Where ids are client-chosen, use a **seeded** hash (seed chosen at startup) so crafted id sequences cannot force collisions, and prefetch the index slot for the next parsed request while the current one matches.
7. **Scale out by sharding instruments across cores.** Each book stays single-threaded, and a symbol router assigns instruments to engine cores. This is how exchanges scale; it avoids any locking on a book.
8. **Build and tuning.**
   - Profile-guided optimization and LTO, and `-march` for the deployment CPU.
   - Verify hot-path code generation (branch layout, no hidden calls) with `perf` and the disassembly.
   - Maintain per-level aggregate quantities, which market-data publication needs and this assignment does not.
9. **Operational requirements a real engine needs:**
   - journaling of inputs for deterministic replay and recovery,
   - risk checks and self-trade prevention,
   - IOC/FOK/market order types,
   - per-session sequence numbers.

## Reproducing

```sh
cmake -S . -B build/release -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_COMPILER=clang++   # or g++
cmake --build build/release -j
taskset -c 2 build/release/bench/matcher_bench            # Linux: pin to one core; ~5 min
build/release/bench/matcher_bench --quick                 # a few seconds
scripts/stress.sh                                          # stress suite (~5 min, a few GB of disk)

# The Linux numbers above, from any host:
docker build -t order-matcher .
docker run --rm order-matcher bash -c 'cmake -S . -B b -G Ninja -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_CXX_COMPILER=clang++-18 && cmake --build b --target matcher_bench && b/bench/matcher_bench'
```

The harness prints the CPU, compiler, flags, timer source and resolution, and seed with every run. Each per-operation scenario times a single engine call and then undoes it untimed, so the book keeps the same shape across samples. Each statistic is the median across 5 runs.
