# Matching Engine Specs

Source LLD: `docs/llds/matching-engine.md`. Terms: *aggressive order* is the incoming AddOrderRequest being processed. *Resting order* is an order already in the book. *Live* means resting in the book right now. *Crosses* means buy price ≥ best ask price, or sell price ≤ best bid price.

## Matching

- [ ] **MATCH-ADD-001**: When an AddOrderRequest's price crosses the best resting price on the opposite side, the engine shall match the aggressive order against opposite-side resting orders.
- [ ] **MATCH-ADD-002**: When selecting the next resting order to match, the engine shall choose the best opposite-side price first (lowest sell price for an aggressive buy, highest buy price for an aggressive sell) and, among resting orders at that price, the one that entered the book earliest.
- [ ] **MATCH-ADD-003**: When an aggressive order matches a resting order, the engine shall trade the smaller of the two orders' remaining quantities at the resting order's price.
- [ ] **MATCH-ADD-004**: While the aggressive order has remaining quantity and its price crosses the best opposite-side price, the engine shall continue matching.
- [ ] **MATCH-ADD-005**: When an AddOrderRequest finishes matching with remaining quantity (including when it matched nothing), the engine shall add that remainder to its own side of the book at its limit price, behind every order already resting at that price.
- [ ] **MATCH-ADD-006**: When an aggressive order is fully filled, the engine shall not add it to the book.
- [ ] **MATCH-ADD-007**: When a resting order is partially filled, the engine shall reduce its quantity in place and keep its time priority within its price level.
- [ ] **MATCH-ADD-008**: When an AddOrderRequest's price does not cross the best opposite-side price, or the opposite side is empty, the engine shall not generate any trade for it.

## Events

- [ ] **MATCH-EVT-001**: For each matched pair of orders, the engine shall emit exactly three events in this order: a TradeEvent, then the aggressive order's fill event, then the resting order's fill event.
- [ ] **MATCH-EVT-002**: When an AddOrderRequest matches several resting orders, the engine shall emit the event triples in the order the resting orders were matched.
- [ ] **MATCH-EVT-003**: When emitting a fill event for either order in a matched pair, the engine shall emit OrderFullyFilled with the order's id if its remaining quantity is zero, and otherwise OrderPartiallyFilled with the order's id and its remaining quantity.
- [ ] **MATCH-EVT-004**: The engine shall emit no events for an AddOrderRequest that does not match, for a CancelOrderRequest, or for any rejected request.

## Cancel

- [ ] **MATCH-CXL-001**: When a CancelOrderRequest names a live order, the engine shall remove that order from the book so that it can never be matched afterwards.

## Rejections

- [ ] **MATCH-REJ-001**: If an AddOrderRequest's orderid equals the id of a live order, then the engine shall reject it with `DuplicateOrderId`, leaving the book unchanged and emitting no events.
- [ ] **MATCH-REJ-002**: If a CancelOrderRequest names an orderid that is not live (never seen, already filled, or already cancelled), then the engine shall reject it with `UnknownOrderId`, leaving the book unchanged.
- [ ] **MATCH-REJ-003**: If an AddOrderRequest has orderid 0 or quantity 0, then the engine shall reject it with `InvalidOrderId` or `InvalidQuantity` respectively, leaving the book unchanged (this is defense in depth behind the parser).
- [ ] **MATCH-REJ-004**: If the book cannot reserve capacity for an AddOrderRequest (allocation failure or the 2^32−1 node limit), then the engine shall reject it with `CapacityExceeded` before matching, leaving the book unchanged and emitting no events.
- [ ] **MATCH-REJ-006**: When an AddOrderRequest has more than one rejection condition, the engine shall report only the first in the order `InvalidOrderId`, `InvalidQuantity`, `DuplicateOrderId`, `CapacityExceeded`.
- [ ] **MATCH-REJ-005**: When an AddOrderRequest reuses the orderid of an order that has been filled or cancelled, the engine shall accept it as a new order with new time priority.

## Safety

- [ ] **MATCH-SAFE-001**: The engine shall require every event sink callback to be `noexcept`, enforced at compile time by the `EventSink` concept.
- [ ] **MATCH-SAFE-002**: The engine shall not let any exception escape from processing an add or cancel request.
