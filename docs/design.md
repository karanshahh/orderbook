# Design notes

## Domain rules (both versions)
- A limit order rests at its price; a market order never rests.
- Better price matches first; at equal price, earlier order first.
- A trade executes at the resting order's price.
- Modify: a pure size reduction at the same price keeps time priority.
  A price change or a size increase is cancel + re-add at the back.
- At rest, best bid < best ask.

## v1
- Bids `std::map<Price, PriceLevel, std::greater<>>`, asks `std::map<Price, PriceLevel>`.
- `PriceLevel`: `std::list<Order>` plus a cached total.
- Index: `unordered_map<OrderId, Locator>`, Locator = {side, price, list iterator}.
- The Locator stores the price, not a map iterator: a level is erased when it
  empties, and a stored iterator to it would dangle. Re-finding by key is safe.

Costs (L = levels on a side, k = orders filled):
add without a fill O(log L); cancel O(1) average + O(log L) to re-find the level;
match O(k) plus O(log L) per level consumed; best price O(1).

## v2
- Pool: `std::vector<Node>` sized at startup; free slots form a singly linked
  list through `Node::next`.
- Queue per level: `Level{head, tail, total}` with `Node::prev/next` as slot
  numbers (32-bit, so a node is 40 bytes).
- Price index: `std::vector<Level>` per side, index = price - min_price.
- Id lookup: `std::vector<uint32_t>`, index = order id, value = slot.
- Best price: an index per side. When the best level empties, walk toward
  worse prices to the next non-empty level (`non_empty` counts make an empty
  side O(1)).
- Events: appended to a caller-owned `std::vector<Event>`; the caller clears it
  and its capacity is reused, so no allocation per call.

Costs: add without a fill O(1); cancel O(1), plus the walk if it empties the
best level; match O(k) plus the walk per level consumed; best price O(1).

Limits, set in `v2::Config` and enforced with rejects: price range, maximum
resting orders, largest order id. `modify` to an out-of-range price is rejected
before the order is touched, so the order is never lost.

## ITCH replay
- One symbol is kept by its stock locate code (from the stock directory 'R').
- Add 'A'/'F' -> add; execute 'E'/'C' and partial cancel 'X' -> size reduction
  (or cancel when nothing is left); delete 'D' -> cancel; replace 'U' -> cancel
  old + add new with the same side.
- ITCH order numbers map to dense ids 1, 2, 3, ... so v2's id table stays small.
- Prices: if every price is a whole cent, the tool uses cents as the tick
  (100x fewer levels). If the price range is still too wide for v2's array,
  orders outside the middle 99.8% of add prices are dropped from the replay for
  both engines, and the count is printed.

## Concurrency
Matching is sequential: each order depends on the state the previous one left.
`LockedBook` (one mutex) exists to measure what sharing costs. Real systems
keep the book on one thread and feed it through a queue.

## Known limits
- Ids are unique among resting orders only; a filled order's id can be reused.
- No self-trade prevention; order types are limit and market only.
- Per-operation timings are limited by the clock's resolution; throughput
  numbers are not.
