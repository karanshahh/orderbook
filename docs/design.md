# Order book v1: design

## Goal
Correct, simple, single-threaded limit order book for one instrument.
Price-time priority. Speed is v2's job.

## Domain rules
- A limit order rests at its price; a market order takes what is available and never rests.
- Better price matches first; at equal price, earlier order first.
- A trade executes at the resting order's price.
- Modify: a pure size reduction at the same price keeps time priority.
  A price change or a size increase is cancel + re-add (back of the queue).
- Invariant: at rest, best bid < best ask.

## Types
Strong types over integers: `Price` (int64 ticks), `Quantity` (uint64), `OrderId` (uint64).
No floating-point prices. `Side` and `OrderType` are `enum class`.

## Data structures
- Bids `std::map<Price, PriceLevel, std::greater<>>`, asks `std::map<Price, PriceLevel>`.
  Best price is `begin()` on both.
- `PriceLevel`: `std::list<Order>` plus cached total.
- `index_`: `unordered_map<OrderId, Locator>`, Locator = {side, price, list handle}.

Why: `std::map` assumes nothing about the price range and keeps levels sorted.
`std::list` erases from the middle in O(1) given an iterator, and erasing one
element never invalidates the others. A `vector` would be O(n) and would
invalidate every stored iterator after the erased element.

Why the Locator stores the price, not a map iterator: a level can be erased
when it empties. A stored map iterator to it would dangle. Re-finding the
level by key is O(log L) and always safe.

## Events
`add`: `{Rejected}` if invalid (zero qty, duplicate resting id); a market order
against an empty side is `{Rejected NoLiquidity}`. Otherwise `{Accepted, Trade...}`;
a limit remainder rests, a market remainder ends with `{Cancelled}`.
`cancel`: `{Cancelled}` or `{Rejected UnknownId}`.
`modify`: `{Modified}` for size-down/same, otherwise the events of the re-add.

## Costs (L = price levels on a side, k = orders filled)
- add, no match: O(log L)
- cancel: O(1) average lookup + O(1) list erase + O(log L) to re-find the level
- match: O(k) fills plus O(log L) per level consumed
- best price: O(1)

## Concurrency
Matching is sequential by nature: each order depends on the book state left
by the previous one. `LockedBook` wraps the book in one mutex so the cost of
locking can be measured. A lock serializes threads; it cannot make matching
parallel. Real systems keep the book on one thread and feed it through a
queue (Sprint 3: SPSC ring buffer).

## Known limits (v1)
- Ids are unique among resting orders only; a filled order's id can be reused.
- Node-based containers allocate on every add (v2 removes this).
- No self-trade prevention, no order types beyond limit and market.
