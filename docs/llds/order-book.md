# Order Book

## Context and Design Philosophy

The order book is the storage layer. It holds resting orders for one instrument and answers four questions cheaply:

- What is the best resting order on a side?
- Where is order *id*?
- Where should a new order at price *p* go?
- How do I remove this order?

It knows nothing about matching rules, messages, or I/O. Those belong to the `MatchingEngine` (`matching-engine.md`), which is the book's only writer.

Guiding principles:

1. **Operations near the best price are the fast path.** Nearly all real activity happens at or near the best price. Operations there are O(1) and touch only a few cache lines. Operations deep in the book may cost O(L), where L is the number of price levels on a side, and the design says so openly.
2. **No allocation in steady state.** Every node lives in a pool that is reused. The only allocations are capacity growth, which is amortized and avoided entirely by reserving capacity up front.
3. **Indices, not pointers.** Every link is a 32-bit index into the pool. Nodes stay small, and when the pool grows and reallocates, no stored handle becomes invalid.
4. **The strong exception guarantee on every mutation.** Any step that can fail, meaning capacity growth, happens *before* any state is modified. A failed operation leaves the book exactly as it was.

## Data Types

| Type | Representation | Notes |
|---|---|---|
| `OrderId` | `uint64_t` | Must be > 0. The value 0 is reserved as the index's "empty slot" marker, and the parser rejects 0 because the brief requires a positive integer. |
| `Quantity` | `uint64_t` | Must be > 0 for any live order. Fill arithmetic is `min`/subtract only, so it cannot overflow. |
| `Price` | fixed-point `int64_t`, 8 decimal places | Defined in `price.md`. Any value is valid, including negative and zero. |
| `Side` | `enum class Side : uint8_t { Buy = 0, Sell = 1 }` | The numeric values match the wire protocol. |
| `NodeIndex` | `uint32_t` | `kNull = UINT32_MAX` is reserved, so a pool can hold at most 2^32 − 1 nodes. |

## Structure

```
OrderBook
 ├─ bids : LevelStore<Buy>    vector<Level>, ascending price  → best (highest) at back()
 ├─ asks : LevelStore<Sell>   vector<Level>, descending price → best (lowest)  at back()
 ├─ pool : NodePool           vector<Node> + free list
 └─ index: OrderIndex         open-addressing OrderId → NodeIndex
```

### Node (pool element)

```cpp
struct Node {            // 32 bytes; two nodes per 64-byte cache line
    OrderId   id;        // 0 for a level sentinel
    Quantity  qty;       // remaining open quantity
    Price     price;     // lets a cancel find its level when the level empties
    NodeIndex next;      // FIFO successor (also the free-list link)
    NodeIndex prev;      // FIFO predecessor
};
```

`static_assert(sizeof(Node) == 32)`. The side is not stored in the node. A cancel learns the side from the index entry instead (see OrderIndex).

### Level

```cpp
struct Level {           // 16 bytes; four levels per cache line
    Price     price;
    NodeIndex sentinel;  // head of a circular doubly-linked FIFO
};
```

Each level's FIFO is a **circular doubly-linked list with a sentinel node** that lives in the pool, the same pattern as the Linux kernel's `list_head`:

- **Oldest** order: `sentinel.next`. **Newest** order: `sentinel.prev`. **Empty** level: `sentinel.next == sentinel`.
- **Why a sentinel:** unlinking any order touches only its two neighbours, so a cancel needs neither the `Level` nor its position in the vector. That makes a non-emptying cancel O(1) with no search at all. Only a cancel that *empties* its level has to find the level (by its price) and erase it.
- **Invariant:** the best level of each side (the vector's `back()`) always has a non-empty FIFO. Interior levels may be empty; see Retained Empty Levels.

### LevelStore\<Side\>

A `std::vector<Level>` sorted so that `back()` is the best price. Bids are sorted ascending and asks descending. An `is_better(a, b)` predicate, chosen at compile time from `Side`, is the only place the ordering is defined.

`reserve_for_add()` also makes sure `size() < capacity()` on the order's own side, growing the vector by doubling if needed, so a later `find_or_insert` never reallocates. The initial reservation is 4096 levels per side.

| Operation | Algorithm | Cost |
|---|---|---|
| `best()` | `back()` | O(1) |
| `pop_best()` | `pop_back()` | O(1) |
| `find_or_insert(price)` | **Fast path:** if the store is empty, or `price` is at least as good as `back().price`, compare with `back()` directly. **Otherwise:** `std::lower_bound` with `is_better`, then `vector::insert` if the price is missing. | O(1) at or better than the best price; otherwise O(log L) search plus an O(d) move, where d is the distance from the insertion point to the best price |
| `erase(price)` | Same search, then `vector::erase` | O(1) at the best price; otherwise O(log L + d). Used only when the retention budget is full |

`Level` is trivially copyable, so the vector's insert and erase compile to `memmove`. Reserved capacity is never released, so after warm-up the vectors do not allocate.

### NodePool

- A `std::vector<Node>` **sized to its full capacity at construction**, so every node is value-initialized and every page has been touched before trading starts. Alongside it are a high-water mark `used_` and a singly-linked free list threaded through `Node::next`.
- **`acquire()`** pops from the free list. If the free list is empty, it takes slot `used_++`.
- **`release(i)`** pushes `i` onto the free list. Memory is never returned to the OS. The pool keeps its peak capacity and reuses freed slots first (see Memory Lifecycle).
- **Growth is a separate, explicit operation, `reserve_for(n)`,** which doubles capacity (`resize`) so that at least `n` more nodes can be acquired. Only `reserve_for` allocates. The matching engine calls it at the *start* of an operation, before any mutation, so both allocation failure and index overflow (more than 2^32 − 1 nodes) happen while the book is still untouched.
- **The initial capacity is set at construction:** by default 2^20 orders (about 32 MB of pool and 32 MB of index), and `--reserve N` on the command line overrides it. A run that stays within the reservation never allocates and never takes a page fault on these structures. Growth past it is a **fallback**, not the expected path.

## Retained Empty Levels

Real books churn at the same prices all day: a level empties when its last order is cancelled, and new orders arrive at that price moments later. Erasing and re-inserting the level each time costs two `memmove`s of every better level. On a mixed flow spanning ~400 levels, profiling showed that as 19% of total runtime.

So a level emptied by a cancel away from the top of the book is **kept in place** and reused:

- **Cancel empties a non-best level:** leave the empty level where it is, O(1), with no search. The side's `empty_levels` count goes up by one.
- **Add at the price of a retained empty level:** `find` locates it by the usual search and the order is appended to its FIFO, with no `memmove`. The count goes down by one.
- **The best level empties** (by a fill or a cancel): pop it, then pop any retained empty levels now at the back, so `back()` is non-empty again and the match check stays one read. Each retained level is popped at most once, so this is amortized O(1).
- **Cap:** a side retains at most `BookConfig::max_retained_levels` empty levels (default 256). When a cancel would exceed it, the emptied level is erased eagerly, exactly as it would be without retention.
  - A cap tied to the number of live levels was considered and rejected: eager erasure lowers the live count, so the bound could break right after a legal operation.
  - The value comes from a measured sweep of 0, 64, 256 and 1024. Deep-level churn gains appear at any nonzero cap: at 10^5 orders, a cancel that empties the deepest level goes from 167 to 7 ns, and re-adding at that price from 184 to 15 ns. Throughput and p99.9 are flat across caps. 256 keeps the worst-case pop run behind the best level short.
- **Capacity:** retained levels are only a cache. If the node pool cannot grow (node cap or allocation failure), `reserve_for_add` releases every retained level and retries before rejecting an order.

**No request costs more than it would without retention,** apart from the pops above, which are bounded by the cap. There is no compaction pass, so no periodic latency spike. Memory is bounded by the cap: one 16-byte `Level` entry and one 32-byte sentinel node per retained level.

## HugePageAllocator

Allocator for the two large arrays, the node pool and the index slots. At about 32 MB each for 10^6 orders, they exceed what the TLB covers with 4 KB pages, and at that size TLB misses cost as much as cache misses.

- **Requests of 2 MiB or more** are allocated 2 MiB-aligned (`std::aligned_alloc`, size rounded up). On Linux, `madvise(MADV_HUGEPAGE)` is called on them **before** the container's value-initialization first touches the memory, so the kernel can back them with 2 MB pages from the first page fault.
- This matters because common Linux defaults, including Ubuntu's, set transparent hugepages to `madvise`: large allocations get hugepages only if they ask for them.
- **Smaller requests** use plain `operator new` (tests with tiny reservations stay on the normal path).
- **On other platforms** `madvise` is skipped: macOS has no transparent hugepages.
- A failed allocation throws `std::bad_alloc` like any allocator, so the existing growth and error paths are unchanged.

## Memory Lifecycle

This follows the industry pattern of **preallocating for the peak, never allocating on the hot path, and never shrinking**:

- **Startup:** the pool, the index, and the level vectors are allocated at their reserved sizes and pre-touched, so page faults happen before the first request instead of during trading. On Linux, the pool and index ask for 2 MB hugepages before that first touch (see HugePageAllocator).
- **Steady state:** freed nodes and index slots are reused. There are no `malloc` calls, `free` calls, or system calls.
- **Growth past the reservation:** capacity doubles, with an O(n) copy and rehash, so that valid input is never rejected just because a default was too small. This is a one-off latency spike, documented here and measured in the benchmark. A deployment that wants hard latency bounds sizes `--reserve` for its peak day.
- **No shrinking:** capacity stays at its peak for the life of the process. This is retained capacity, not a leak: every byte is still owned and reused. Returning memory to the OS and taking it back later would put system calls and page faults on the hot path. In production the peak is bounded by the trading session, because futures venues have a daily maintenance break (for example, CME Globex halts for about an hour each evening), when engines are restarted or reinitialized.

### OrderIndex

- An open-addressing hash table with linear probing. Capacity is a power of two, and the load factor is kept at or below 0.5.
- **Slot:** `{ OrderId key; NodeIndex node; Side side; }`, 16 bytes. `key == 0` marks an empty slot, which is safe because ids are always positive.
- **Hash: blocked Fibonacci with a pre-xor.** The id's low 2 bits choose a slot within a 4-slot block, which is exactly one 64-byte cache line of 16-byte slots. The remaining bits choose the block: `x = id >> 2; x ^= x >> 12; block = (x * 0x9E3779B97F4A7C15) >> (64 - log2(capacity) + 2)`, and `slot = block * 4 + (id & 3)`.
  - **Locality:** sequential ids, which is how exchanges assign them, fill each cache line 4 at a time instead of touching 4 random lines.
  - **No collisions for sequential ids:** Fibonacci hashing spreads consecutive inputs almost perfectly evenly, and the pre-xor only permutes ids within aligned runs, so that property survives.
  - **Robust to strides:** the pre-xor feeds high bits into the low bits that power-of-two strides leave zero. Plain Fibonacci hashing is weak there.
- **Deletion:** backward-shift. No tombstones are used, so probe lengths stay short under heavy add/cancel churn, and a lookup never has to skip over dead entries.
- **Growth:** doubling plus a full rehash, triggered by `reserve_for(n)` before any mutation, just like the pool. It is O(n) but happens only during warm-up. With a pre-reserved size it never happens.
- **Operations:**
  - `find(id) → optional<Entry>`
  - `insert(id, node, side)`. The precondition is that `id` is absent; the engine checks that first.
  - `erase(id)`

## Public Interface (consumed by MatchingEngine)

```cpp
class OrderBook {
public:
    explicit OrderBook(const BookConfig& = {});    // reserve_orders (2^20), reserve_levels (4096), max_nodes (2^32−1)

    // Capacity: call before mutating. Returns false without side effects on failure.
    // Ensures room for 1 order node + 1 level sentinel + 1 index slot + 1 level on `side`.
    [[nodiscard]] bool reserve_for_add(Side side);

    // Queries
    [[nodiscard]] bool  contains(OrderId) const;
    [[nodiscard]] bool  empty(Side) const;
    [[nodiscard]] Price best_price(Side) const;        // precondition: !empty(side)

    // Mutations (preconditions checked in debug builds via assert)

    // Fill up to `max_qty` against the oldest order at the best price on `side`.
    // Reduces that order in place (keeping its queue position), or removes it,
    // and its level, if it reaches zero. Precondition: !empty(side), max_qty > 0.
    struct Fill { OrderId resting_id; Price price; Quantity qty; Quantity resting_remaining; };
    [[nodiscard]] Fill fill_best(Side side, Quantity max_qty);

    void rest(Side, OrderId, Quantity, Price);    // append to the back of its (possibly new) level
    [[nodiscard]] bool cancel(OrderId);           // false if not live; unlinks, drops level if emptied

    // Verification support (tests only, O(n))
    void check_invariants() const;                 // aborts with a description on violation
    std::vector<LevelSnapshot> snapshot(Side) const;  // best → worst, oldest first; allocates
};
```

## Invariants

These are checked by `check_invariants()` after every request in the randomized tests.

1. Each `LevelStore` is strictly ordered by `is_better`, with no duplicate prices.
2. Every level's FIFO is circular and consistent in both directions (`n.next.prev == n`). Each side's best level is non-empty. The number of empty levels on a side equals that side's retained count and never exceeds the cap.
3. Every order node in a level's FIFO has `price == level.price`, `qty > 0`, and `id != 0`.
4. The index holds exactly the set of live orders. Each entry's node has the same id, and the entry's side matches the side of the level that holds the node.
5. The book is not crossed: `!(best_bid >= best_ask)` whenever both sides are non-empty.
6. The live node count, meaning the pool's size minus its free list, equals the number of live orders plus the number of levels on both sides, retained empty levels included (one sentinel per level).

## Decisions & Alternatives

| Decision | Chosen | Alternatives Considered | Rationale |
|---|---|---|---|
| Level container | Sorted `vector`, best at back | `std::map`; dense tick ladder | HLD decision 1. |
| Interface granularity | Intention-level operations (`fill_best`, `rest`, `cancel`). `Node` is private. | Exposing `Node&` and letting the engine mutate quantities | The book owns its invariants, and no caller can leave it inconsistent. The engine decides *what* happens and the book decides *how*. Everything is inlined, so the boundary costs nothing at runtime. |
| Per-level FIFO | Circular intrusive list with a pool-resident sentinel | head/tail stored in `Level`; `std::list`; `std::deque` | If head/tail lived in `Level`, cancelling the first or last order of a level would need the level's position in the vector, but that position changes as levels are inserted and erased. So every cancel would need an O(log L) search. The sentinel makes non-emptying cancels O(1) and search-free. |
| Links | 32-bit indices | Raw pointers | Pointers would be invalidated when the pool reallocates. Indices halve the link size and keep `Node` at 32 bytes. |
| Level aggregate quantity | **Not stored** | a `total_qty` field on `Level` | The brief never needs it. It would cost an update on every fill and cancel, and a sum of `uint64` quantities can overflow. The HLD invariant "each level's stored quantity equals the sum of its orders" is replaced by invariants 2 and 3 above. |
| Side lookup on cancel | Stored in the index slot | Stored in the node; searching both sides | It fits in the slot's padding for free, and the node stays at 32 bytes. |
| Hash function | Blocked Fibonacci with pre-xor (`x ^= x >> 12`), 4-id blocks (one cache line) | Plain Fibonacci; identity (`id & mask`); 8- and 16-id blocks; a multiply-fold-multiply block mixer; a seeded hash such as wyhash | Measured at 10^6 resting orders (Linux, Clang 18, mean ns): **plain Fibonacci** gives 52 for a sequential-id add, 67 for a random cancel, 22 for a cancel in id order, and 41 / 102 for strided ids (add / random cancel). **Identity** is fastest for sequential ids (15 / 120 / 8) but collapses on strided ids (970 / 1,492): a denial-of-service risk. **16-id blocks** give 19 / 117 / 13, because longer probe clusters slow random cancels. **4-id blocks** give 30 / 74 / 18, but plain Fibonacci block selection spreads some power-of-two strides poorly: random cancels with ids strided by 2^18 take 279. A **multiply-fold-multiply** mixer fixes strides (80) but turns sequential ids into random placement, with 28,000 of 131,000 colliding and random cancels at 136. The chosen **pre-xor** keeps zero sequential collisions and a worst stride spread of 8,497 of 10,000 blocks (plain Fibonacci: 6,855). Measured: sequential add 29, cancel in id order 18, and random cancels 75–77 for sequential, random and strided (2^12, 2^18, 2^20) ids alike. A seeded hash is a production item (see Open Questions). |
| Index deletion | Backward-shift | Tombstones | Tombstones pile up under add/cancel churn and slow down every probe. |
| Capacity growth | An explicit `reserve_for` before mutation | Implicit growth inside `acquire`/`insert` | This gives the strong exception guarantee and puts the only allocation in one place in the code. |
| Pool storage | `std::vector<Node, HugePageAllocator<Node>>`, sized and touched at startup | Plain `std::vector`; a virtual-address reservation with `mmap`; a chunked pool | `vector` is portable and easy to read, and pre-sizing it gives the page-touching of preallocation. The allocator asks Linux for transparent hugepages *before* the first touch (see HugePageAllocator). The `mmap` reservation, which grows without moving, remains a production item. |
| Growth past the reservation | Double capacity (fallback) | Reject with `CapacityExceeded`; chunked pool with no copy; incremental rehash | Rejecting valid input because a default was too small would violate "no input is mishandled". Chunked pools and incremental rehashing remove the spike but add indirection and complexity for a path that preallocation already avoids. They go in `PERFORMANCE.md`. |
| Returning memory | Never; keep the peak | Shrink-to-fit or compaction during quiet periods | Compaction would move live nodes, which invalidates their indices and means rewriting every link and index entry. That is risky code with no benefit when the process restarts each trading session. |

## Open Questions & Future Decisions

### Resolved
1. ✅ Where orders live: in one pool shared by both sides, since the side is known from the level or the index.
2. ✅ Price equality between levels is exact integer equality (fixed-point).

### Deferred
1. A seeded hash, with the seed chosen at startup, so crafted order ids cannot force collisions (a denial-of-service vector). This goes in the `PERFORMANCE.md` production section.
2. For production: a virtual-address reservation (`mmap` with `MAP_NORESERVE`) so the pool can grow without moving, explicit hugetlbfs pages where transparent hugepages are disabled, `mlock` to keep the pool in RAM, and incremental rehashing to remove the index's growth spike. This goes in `PERFORMANCE.md`.
3. A dense tick ladder as an alternative `LevelStore` (stretch goal #1). The `LevelStore` interface above is the seam where it would plug in.

## References

- `docs/high-level-design.md`: Key Design Decisions 1–3
- `docs/llds/matching-engine.md`: the only writer to this component
- Linux kernel `include/linux/list.h`: the circular sentinel list pattern
