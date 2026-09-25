# Matching Engine

## Context and Design Philosophy

The matching engine applies the brief's rules. It takes typed requests (`AddOrder`, `CancelOrder`), changes the `OrderBook` (`order-book.md`), and reports what happened as typed events to a sink. It does not parse text, format output, or touch streams (see HLD decision 5).

Guiding principles:

1. **The code reads like the brief.** The matching loop should map sentence-for-sentence onto the brief's Logic section: price first, then age; trade at the resting order's price for the smaller quantity; keep going until the aggressive order is filled or no longer crosses; rest the remainder.
2. **A request is applied completely or not at all.** All validation and capacity reservation happen before the first event is emitted or any state changes. A rejected request emits nothing and leaves the book unchanged.
3. **Rejections are return values, not exceptions.** Rejecting bad input is normal operation, so it goes through the normal control flow. The caller (the app) turns a rejection into a stderr diagnostic.

## Interface

```cpp
struct AddOrder    { OrderId id; Side side; Quantity qty; Price price; };
struct CancelOrder { OrderId id; };

struct Trade                { Quantity qty; Price price; };
struct OrderFullyFilled     { OrderId id; };
struct OrderPartiallyFilled { OrderId id; Quantity remaining; };

// Sink callbacks must be noexcept: an exception mid-sweep would leave the book
// half-updated. Sinks that can fail (e.g. stdout) record the error internally
// and let the caller check it between requests.
template <class S>
concept EventSink = requires(S s, const Trade& t, const OrderFullyFilled& f,
                             const OrderPartiallyFilled& p) {
    { s.on_trade(t) } noexcept;
    { s.on_fully_filled(f) } noexcept;
    { s.on_partially_filled(p) } noexcept;
};

enum class Reject : uint8_t {
    None = 0,
    InvalidOrderId,     // id == 0          (defense in depth; parser also rejects)
    InvalidQuantity,    // qty == 0         (defense in depth; parser also rejects)
    DuplicateOrderId,   // id is live in the book
    UnknownOrderId,     // cancel of an id that is not live
    CapacityExceeded,   // pool/index cannot grow (2^32-1 node limit or allocation failure)
};

template <EventSink Sink>
class MatchingEngine {
public:
    MatchingEngine(Sink& sink, std::size_t reserve_orders);
    [[nodiscard]] Reject add(const AddOrder&);
    [[nodiscard]] Reject cancel(const CancelOrder&);
    const OrderBook& book() const;   // read-only, for tests and invariant checks
};
```

The engine holds a reference to the sink and calls it synchronously. The engine does not own the sink, so the app, the tests, and the benchmark can each supply their own.

## Add Algorithm

```
add(req):
  1. validate    id == 0 → InvalidOrderId; qty == 0 → InvalidQuantity
                 book.contains(id) → DuplicateOrderId
  2. reserve     book.reserve_for_add(req.side) fails → CapacityExceeded
                 (the only step that can allocate; nothing has changed yet)
  3. match       opp = opposite(req.side); remaining = req.qty
                 while remaining > 0 and !book.empty(opp)
                                     and crosses(req.side, req.price, book.best_price(opp)):
                     f = book.fill_best(opp, remaining)     // book keeps its own invariants
                     remaining -= f.qty
                     emit Trade{f.qty, f.price}                                     (1)
                     emit remaining == 0 ? FullyFilled{req.id}
                                         : PartiallyFilled{req.id, remaining}       (2)
                     emit f.resting_remaining == 0 ? FullyFilled{f.resting_id}
                                         : PartiallyFilled{f.resting_id, f.resting_remaining} (3)
  4. rest        if remaining > 0: book.rest(req.side, req.id, remaining, req.price)
  return None
```

The order of the checks in steps 1–2 is also the **rejection precedence**: `InvalidOrderId` → `InvalidQuantity` → `DuplicateOrderId` → `CapacityExceeded`. The cheap checks come first, and the only step that can allocate comes last.

`crosses(Buy, p, best_ask) = p >= best_ask` and `crosses(Sell, p, best_bid) = p <= best_bid`. Equal prices cross, matching the brief's "matches or exceeds".

Properties that follow directly from the algorithm:

- **Output order per matched pair is (1) Trade, (2) aggressive fill, (3) resting fill,** exactly as the brief requires. Pairs are emitted in the order the resting orders were matched.
- **Every Trade comes with at least one FullyFilled,** because `q = min(...)` drives at least one side to 0, **and at most one PartiallyFilled.** Both properties are stated in the brief.
- **The trade price is always the resting order's price**, never the aggressive order's limit price.
- **The aggressive order is never visible in the book while it matches,** so it cannot match against itself.
- **Price improvement:** a buy at 1050 against asks at 1025 trades at 1025.
- **Termination:** each iteration either fully fills the aggressive order, which ends the loop, or fully fills and removes one resting order. So the loop runs at most once per resting order plus once.
- **An aggressive order that is fully filled never touches its own side of the book and never enters the index.**
- **A partially filled resting order keeps its place in the queue.** Its quantity is reduced in place, and it is not moved to the back. This follows the brief: S5 becomes S4, and a later sell at 1025 queues *behind* S4.
- **Time priority belongs to the order, not to the id.** Once an id has filled or been cancelled, it can be reused. The new order is a fresh order and joins the *back* of its level.
- **No exceptions escape a request.** Validation returns `Reject`, capacity failure returns `CapacityExceeded`, and sink callbacks are `noexcept`. So a request can never be interrupted partway through.

## Cancel Algorithm

```
cancel(req):
  1. book.cancel(req.id) returns false → UnknownOrderId
     (O(1) unlink; the level is erased only if it becomes empty)
  return None
```

A cancel emits **no** events. The brief defines output messages only for trades and fills, and HLD Non-Goals rules out acknowledgement messages.

## Complexity (the three paths the brief asks about)

In this table, L is the number of price levels on a side, d is the distance in levels from the affected level to the best price, and k is the number of resting orders the add fills.

| Path | Cost | Why |
|---|---|---|
| **Does an add match?** | **O(1)** | One `empty()` check plus one comparison against `back().price` on the opposite side |
| **Remove a filled resting order** | **O(1)** | Unlink the oldest node of the best level, erase it from the index, and release it to the pool. If the level empties, `pop_back` it and release its sentinel. |
| **Cancel** | **O(1)** when the level stays non-empty. **O(log L + d)** when the cancel empties its level. | The sentinel list gives an O(1) unlink. Only an emptied level needs a search by price and a `vector::erase` (a `memmove` of d × 16 bytes). |
| Add that rests without matching | O(1) at or better than the best price; otherwise O(log L + d) if a new level is created, and O(log L) to find an existing level | See `LevelStore::find_or_insert` |
| Add that sweeps levels | O(k) plus one O(1) removal per emptied level | Each fill and each emptied level is O(1) |

**Which paths are favored:** operations at or near the best price, which is where almost all traffic goes. Deep-book level creation and deletion pay O(d) data movement instead of pointer-chasing through a tree. `PERFORMANCE.md` measures this trade-off rather than just asserting it.

## Decisions & Alternatives

| Decision | Chosen | Alternatives Considered | Rationale |
|---|---|---|---|
| Sink binding | Template parameter constrained by the `EventSink` concept | Virtual interface; `std::function`; returning a `vector<Event>` | Calls to a template parameter can be inlined, with zero dispatch cost. The concept documents the contract and gives clear compile errors. Returning a vector would allocate, or else need a capacity policy. |
| Reporting rejections | `[[nodiscard]] Reject` return value | Exceptions; a separate reject callback on the sink | Rejecting input is expected, not exceptional. `[[nodiscard]]` makes it impossible to ignore silently. Keeping rejections off the sink keeps the sink's contract equal to the brief's three output messages. |
| Engine re-validates id and quantity | Yes (`InvalidOrderId`, `InvalidQuantity`) | Trust the parser | The engine is a library with its own contract, and an id of 0 would corrupt the index's empty-slot marker. It costs two predictable branches. |
| When a partial fill of the aggressive order is reported | After every trade, with the running remainder | Only once at the end | The brief's example output (`4,1000008,1`) requires a report after every trade. |
| Handling a duplicate id | Reject; the book and existing order are unchanged | Replace the existing order; treat as modify | The brief says ids are unique. Silently changing an existing order would be surprising and unsafe. |

## Open Questions & Future Decisions

### Resolved
1. ✅ Adds are rejected when their id is live. Reuse of an id that already filled or was cancelled is **accepted** (HLD FAQ: unbounded memory otherwise). Phase 4 confirms this.
2. ✅ Cancels produce no stdout output.

### Deferred
1. Self-trade prevention is out of scope (HLD Non-Goals). There is no participant identity.
2. Market, IOC, FOK, and modify orders are out of scope (HLD Non-Goals). If added, they would be new request types with their own `Reject` codes; the matching loop would stay as it is.

## References

- `docs/high-level-design.md`: Approach, Key Design Decisions 5–6
- `docs/llds/order-book.md`: storage and complexity details
- The brief's Logic and Assignment sections (confidential; not in the repo)
