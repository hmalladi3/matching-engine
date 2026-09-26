# Order Book Specs

Source LLD: `docs/llds/order-book.md`. Notation: L is the number of price levels on one side, and d is the distance in levels from an affected level to the best price on that side.

## Operations and Cost

- [x] **BOOK-OP-001**: The book shall return the best price of either side in O(1) by reading the last element of that side's sorted level vector.
- [x] **BOOK-OP-002**: When `fill_best` fully fills the oldest order at the best price, the book shall remove that order, and its level if the level becomes empty, in O(1).
- [x] **BOOK-OP-003**: When `fill_best` partially fills the oldest order at the best price, the book shall reduce its quantity in place without changing its position in the level's FIFO.
- [x] **BOOK-OP-004**: When `cancel` removes an order whose level still contains other orders, the book shall unlink it in O(1) without searching the level vector.
- [x] **BOOK-OP-005**: When `cancel` removes the last order of a level, the book shall erase that level in O(log L + d).
- [x] **BOOK-OP-006**: When `rest` adds an order at a price at least as good as its side's current best price, or to an empty side, the book shall find or create the level in O(1) without a binary search.
- [x] **BOOK-OP-007**: When `rest` adds an order at a price worse than its side's current best price, the book shall find the level by binary search and, if the price is new, insert a level while keeping the vector ordered.
- [x] **BOOK-OP-008**: When `rest` adds an order, the book shall append it to the back of its level's FIFO.
- [x] **BOOK-OP-009**: The order index shall support lookup, insert, and erase by orderid in expected O(1), using linear probing with backward-shift deletion (no tombstones).

## Memory

- [x] **BOOK-MEM-001**: When constructed with a reservation of N orders (default 2^20), the book shall allocate and touch the node pool and order index for N orders and reserve 4096 levels per side before processing any request.
- [x] **BOOK-MEM-002**: While the number of live orders and levels stays within reserved capacity, the book shall perform no heap allocation during add, fill, or cancel operations.
- [x] **BOOK-MEM-003**: When an add needs capacity beyond the current reservation (for its node, level sentinel, index slot, or level entry), the book shall grow the affected structure by doubling before any mutation of book state.
- [x] **BOOK-MEM-004**: If growth fails (allocation failure or exceeding 2^32−1 pool nodes), then `reserve_for_add` shall return false and leave the book unchanged.
- [x] **BOOK-MEM-005**: When an order is removed, the book shall return its node and index slot for reuse and shall not release memory to the operating system.

## Invariants

- [x] **BOOK-INV-001**: `check_invariants` shall verify, and abort with a description on violation, that each side's levels are strictly ordered with no duplicate prices, that every level's FIFO is non-empty and consistent in both directions, that every order node has its level's price, positive quantity and non-zero id, that the index holds exactly the live orders with the correct side, that the book is not crossed, and that live pool nodes equal live orders plus levels.
