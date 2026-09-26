# Performance

<!-- @spec DLV-PERF-003 -->

This document covers how fast the three request paths the assignment asks about are, which paths the design favors and why, the trade-offs made, and what would change for production. The numbers come from `bench/matcher_bench`. [How to reproduce](#reproducing) is at the end.

## Summary

In the table below, L is the number of price levels on one side of the book and d is the distance in levels from the affected level to the best price. Measured values are the mean ns per complete request (Linux, Clang 18 `-O3`) on a book of 100,000 resting orders with 1,000 levels per side. Full tables for both compilers are in [Results](#results).

| Path | Cost | Measured |
|---|---|---|
| **Does an AddOrderRequest match?** | **O(1)**: compare its limit with `back()` of the opposite side's level vector | Part of every add: **~21 ns** for an add that checks for a match and then rests |
| **Removing a filled order** | **O(1)**: unlink the oldest node of the best level, erase it from the id index, return it to the pool; `pop_back` the level if it empties | **~23 ns** for an add that fully fills one resting order; **~10 ns per order** within a multi-level sweep |
| **Removing a cancelled order** | **O(1)** to find it (hash) and unlink it; **+O(log L + d)** only if the cancel empties its level | **~14 ns** (at the best or deepest level), ~38 ns for a random order; emptying the best level ~17 ns; emptying the deepest of 1,000 levels ~200 ns |

All three paths make **zero heap allocations** and **zero system calls** in the steady state; a dedicated test enforces this. End to end, the binary processes **about 9.6 million messages per second** on one core, including reading, parsing, matching and formatting. At 10^7 messages through the real binary it sustains 7.7–8.5M messages/s in 68 MiB of memory (stress test S1).

## Design, and why each path is cheap

```
OrderBook
 ├─ bids : LevelStore<Buy>   std::vector<Level>, ascending  → best bid = back()
 ├─ asks : LevelStore<Sell>  std::vector<Level>, descending → best ask = back()
 ├─ pool : NodePool          std::vector<Node> preallocated; free list; 32-bit indices
 └─ index: OrderIndex        open addressing, linear probing, Fibonacci hash, load ≤ 1/2
Level = {price, sentinel}   16 bytes: four levels per cache line
Node  = {id, qty, price, next, prev}   32 bytes: two nodes per cache line
```

**Match detection.** The best opposite price is always the last element of a contiguous vector, so the check is one load and one integer compare. Prices are fixed-point `int64` (8 decimal places), so the compare is exact and a single instruction. There is no floating point anywhere in the engine.

**Filled-order removal.** Each level keeps its orders in a circular doubly-linked FIFO threaded through the pool nodes, with a sentinel node, the same pattern as the Linux kernel's `list_head`. The oldest order is `sentinel.next`. Removing it touches its two neighbours, one index slot and the pool's free-list head. If the level empties, it is the vector's last element, so it is removed with `pop_back`. A sweep through k resting orders therefore costs O(k), with constant work per fill.

**Cancel.** The id index maps an order id to its pool node in expected O(1). Most lookups touch one cache line: the table is at most half full, and linear probing keeps collisions adjacent. Because the FIFO is circular through a sentinel, **unlinking an order never needs to know which level it belongs to**. Only a cancel that removes a level's *last* order has to locate the level: a binary search by price, then a `vector::erase`, which is a `memmove` of the d levels that are better than it. Deletion from the index uses backward shift instead of tombstones, so probe lengths do not degrade under heavy add/cancel churn.

**Memory.** At startup the pool and index are sized for `--reserve N` orders (default 2^20, about 64 MB) and every page is touched, so no page faults happen while trading. After that, freed nodes and slots are reused (most recently freed first, since that memory is still in cache). Nothing on the request path calls `malloc`. Capacity is checked **before** any mutation, so a request is always applied completely or rejected with no effect, even if growth fails.

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
| Doubling growth past the reservation | Never rejects valid input just because a default was too small | One-off O(n) copy/rehash spike: the slowest single add while growing to 10^6 orders took 2.4 ms (p99.9 stays at 250 ns) | `--reserve`; production options below |
| Capacity reserved *before* matching | Every request is all-or-nothing, even when allocation fails | An add that would have freed capacity by filling can be rejected when the book is at its hard limit | Only reachable at the node limit (2^32 − 1) or on allocation failure |
| Unseeded Fibonacci hash | One multiply, excellent spread for sequential ids | Crafted ids could force collisions (a denial-of-service vector) | Production: a seeded hash (below) |
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
| add, rests at best (no match) | 17.6 (42) | 22.0 (83) | 123 (208) | | 23.6 (42) | 124 (208) |
| add, creates new best level | 17.1 (42) | 20.6 (42) | 120 (208) | | 24.8 (42) | 125 (208) |
| add, creates level behind all others | 40.9 (83) | 215 (375) | 303 (417) | | 196 (250) | 298 (375) |
| add, fully fills one resting order | 17.1 (42) | 22.5 (42) | 124 (208) | | 23.6 (42) | 122 (208) |
| add, sweeps 1 level | 71.5 (83) | 366 (417) | 5,831 (7,125) | | 325 (417) | 5,746 (7,042) |
| add, sweeps 10 levels | 608 (667) | 3,654 (4,542) | 58,660 | | 3,201 (3,958) | 59,375 |
| cancel, order at best level | 11.8 (42) | 13.8 (42) | 26.6 (42) | | 20.0 (42) | 31.4 (42) |
| cancel, order at deepest level | 11.1 (42) | 13.7 (42) | 26.5 (42) | | 19.7 (42) | 34.6 (42) |
| cancel, order at random level | 12.1 (42) | 37.7 (125) | 269 (458) | | 39.9 (125) | 289 (458) |
| cancel, empties best level | 13.1 (42) | 16.6 (42) | 17.4 (42) | | 23.1 (42) | 24.5 (42) |
| cancel, empties deepest level | 34.0 (42) | 204 (250) | 205 (250) | | 198 (250) | 197 (250) |

Sweep cost is proportional to the orders filled: 1 level holds 10, 50 and 500 orders at the three sizes, so a fill costs **~7–12 ns per resting order** throughout.

### Deep-book worst case: level created and removed behind every other level

| Levels per side | 100 | 1,000 | 10,000 | 100,000 |
|---|---|---|---|---|
| add (mean ns, Clang / GCC) | 57 / 46 | 217 / 200 | 1,769 / 1,688 | 25,142 / 23,005 |
| cancel (mean ns, Clang / GCC) | 53 / 45 | 206 / 191 | 1,740 / 1,661 | 24,690 / 22,762 |

This is linear in L, exactly the O(d) `memmove` the design accepts. It stays under 250 ns for books up to ~1,000 levels, which covers typical futures books.

### Latency on generated order flow (engine only, per request)

| Profile | p50 | p99 | p99.9 | mean (Clang / GCC) |
|---|---|---|---|---|
| tight (narrow band, heavy matching) | 42 | 167 | 250 | 58 / 64 |
| mixed (all behaviors, mid through zero) | 42 | 167 | 292 | 68 / 71 |
| sweep (large aggressive orders) | 42 | 208 | 500 | 61 / 66 |

These include one timer read per request. In batch timing (below) the same engine work costs 27–35 ns per request.

### Throughput (batch timing, 5×10^6 messages)

| Stage | Clang 18 | GCC 13 |
|---|---|---|
| parse only | 34–36 ns/msg | 35–36 ns/msg |
| engine only (pre-parsed) | 27–35 ns/msg | 30–43 ns/msg |
| format only (1 trade + 2 fills) | 22 ns | 29 ns |
| **end to end (`run_app`, in memory)** | **103–105 ns/msg = 9.6–9.7M msg/s** | **117–120 ns/msg = 8.4–8.5M msg/s** |

**Compiler choice:** Clang is **about 12% faster end to end**, mostly in output formatting and in the engine on the mixed profile. GCC is marginally faster on a few deep-level operations. The recommended build is therefore Clang, based on these measurements.

### Stress suite (real binaries; `scripts/stress.sh`)

| Test | Result |
|---|---|
| S1: 10^7 requests × 3 profiles | 7.7–8.5M msg/s (file in, file out), 68 MiB peak; stdout and stderr **byte-identical** across Clang, GCC and a rerun |
| S2: 5×10^6 orders, 10^5 levels per side; cancel half; sweep both sides | Exactly the expected 2.6M trades, book empty afterwards, 4.0 s, 649 MiB peak (pool and index grew from the 2^20 default) |
| S3: 3×10^7-request soak | Memory flat at 67 MiB from 25% of the run to the end |
| S4: a 1 GiB single line | One diagnostic, trailing trade correct, **3 MiB peak**: the line is streamed and never buffered |
| S4: 10^7 blank, comment, garbage and unknown-cancel lines | Exactly one diagnostic per bad line, 0.8–0.9 s per 10^7 lines, 3 MiB peak |
| S5: stdout through a throttled reader | 9 MB of output byte-identical to the unthrottled run |

### Same hardware without the VM (macOS 15, Apple Clang 17, native)

| Mean ns per request | 10^5 orders | 10^6 orders |
|---|---|---|
| add, rests at best | 10.0 | 47.0 |
| add, fully fills one order | 10.4 | 53.6 |
| cancel, order at best level | 6.5 | 8.1 |
| cancel, order at random level | 9.8 | 100.9 |
| cancel, empties deepest level | 166 | 166 |
| end to end (`run_app`, in memory) | 107–111 ns/msg = 9.0–9.4M msg/s | |

Natively, the pure-CPU operations run about **twice as fast** as in the Linux VM, and the 10^6-order penalty shrinks from ~120 ns to ~47 ns. Inside a VM every TLB miss needs a two-stage page-table walk, so the gap is itself evidence that the large-book cost is address translation and cache misses, not algorithmic work. It is also why hugepages are near the top of the production list. End-to-end throughput is similar on both, since it is dominated by parsing and formatting, which are CPU-bound.

### What the numbers say about the design

- **Top-of-book operations are flat as the book grows until the working set leaves the cache.** At 10^3 and 10^5 resting orders, adds, fills and cancels cost 11–24 ns.
- **At 10^6 orders, anything touching a *random* order costs 120–290 ns.** The algorithm is unchanged; the cost is memory. Pool plus index is ~64 MB, larger than the last-level cache. A fresh or random id lands in a random index slot, costing one DRAM miss (~100 ns) plus TLB misses on 4 KB pages. Cancels confined to a few levels stay at ~27 ns because their slots stay cached.
- **A trade-off made visible here: Fibonacci hashing makes the index robust to strided ids but scatters sequential ids.** Exchange-assigned order ids are usually sequential, so production would index them directly (no hash, perfect locality), or keep the hash and add hugepages plus software prefetch (see below).
- **The O(d) deep-level cost behaves exactly as predicted,** and parsing, not matching, is the largest share of end-to-end time (~35 of ~105 ns).

## What I would change for production

Roughly in order of expected impact for a real futures venue or trading system:

1. **Dense price ladder (stretch goal #1).** A futures contract has a known tick size and a bounded daily price range. That allows an array indexed by `(price − base) / tick`, a hierarchical bitmap of non-empty levels, and `lzcnt`/`tzcnt` to find the next best level. Every level operation becomes O(1), including the deep-book cases that cost O(d) here. The `LevelStore` interface is the seam where it plugs in; the rest of the book is unchanged. I did not do it here because the assignment allows any decimal price with no tick size.
2. **Binary protocol instead of CSV.** Parsing is the largest single cost in the pipeline (~35 ns of ~105 ns per message). A fixed-layout binary format (SBE or ITCH-style) turns parsing into a few loads.
3. **Kernel-bypass networking and a busy-polling pinned thread.** In production, input comes from the network, not stdin. The standard stack is Solarflare/AMD `ef_vi` or Onload, or DPDK, with the matching thread pinned to an isolated core (`isolcpus`, `nohz_full`, IRQ affinity) busy-polling its queue so it never sleeps.
4. **Pipeline with the LMAX Disruptor pattern (stretch goal #2).** Reader/decoder → engine → encoder/publisher on separate cores, connected by single-producer/single-consumer ring buffers with cache-line-padded sequence counters. The engine thread then does only matching. This raises throughput at the cost of one cross-core hop (~50–100 ns) per message, so it is worth it only when decoding and encoding cost as much as matching, which the numbers above suggest they do.
5. **Memory (stretch goal #3).**
   - Back the pool with a large virtual-address reservation (`mmap` with `MAP_NORESERVE`), so growth never copies or moves nodes.
   - Use 2 MB hugepages to cut the TLB misses that dominate at 10^6 resting orders, and `mlock` to keep the book resident.
   - Allocate on the NUMA node of the matching core.
   - Rehash incrementally (as Redis does) to remove the index's growth spike.
6. **Order-id indexing for the real id scheme.** Venue-assigned ids are typically sequential per session, so they can index a slab directly (id − session base): no hashing, perfect locality, and none of the DRAM misses measured at 10^6 orders. Where ids are client-chosen, use a **seeded** hash (seed chosen at startup) so crafted id sequences cannot force collisions, and prefetch the index slot for the next parsed request while the current one matches.
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
