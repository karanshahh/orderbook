# orderbook

A limit order book and matching engine for one instrument, in C++23.
It matches orders by price, then by arrival time (price-time priority),
in two versions: a simple one built on standard containers (v1) and a
fast one with no allocation on the hot path (v2). Both are checked
against each other and replayed on real Nasdaq market data.

## Contents

- [What an order book does](#what-an-order-book-does)
- [API](#api)
- [v1: standard containers](#v1-standard-containers)
- [v2: built for speed](#v2-built-for-speed)
- [Real market data](#real-market-data)
- [How correctness is checked](#how-correctness-is-checked)
- [Build, test, run](#build-test-run)
- [Results](#results)
- [Repository layout](#repository-layout)

## What an order book does

An exchange keeps, for each stock, a list of everyone willing to buy (bids)
and sell (asks), sorted by price. When a new order arrives:

- If it can trade with orders already waiting, it does. Best price first;
  at the same price, whoever arrived first.
- Each trade happens at the waiting (resting) order's price.
- A limit order that is not fully filled waits in the book at its price.
  A market order never waits: whatever it can't fill is dropped.

The book is never left crossed: the best bid is always below the best ask.

## API

Both versions expose the same operations and produce the same events.

| Function | What it does |
|---|---|
| `add(order)` | Submit a limit or market order. Matches what it can, rests the rest (limit only). |
| `cancel(id)` | Remove a resting order. |
| `modify(id, price, qty)` | Reduce size at the same price, keeping its place in line. Any other change is cancel + re-add at the back. |
| `best_bid()`, `best_ask()` | Best prices, or none if that side is empty. |
| `volume_at(side, price)` | Total resting quantity at one price. |
| `open_orders()`, `levels(side)` | Number of resting orders; number of non-empty prices on a side. |
| `find(id)` | The resting order with that id, if any. |
| `crossed()` | True if best bid >= best ask. Used by tests; must always be false. |

Every call returns events, in order:

| Event | Meaning |
|---|---|
| `Accepted` | The order passed validation. |
| `Trade` | Resting order id, incoming order id, price, quantity. |
| `Cancelled` | Removed, with the quantity that was left. Also ends a market order that ran out of liquidity. |
| `Modified` | Size reduced in place. |
| `Rejected` | Not accepted, with a reason: zero quantity, duplicate id, unknown id, no liquidity (market order on an empty side), or a v2 limit (price out of range, id out of range, book full). |

Prices and quantities are integers (`Price` is ticks, never floating point).
`Price`, `Quantity` and `OrderId` are separate types, so they can't be mixed up.

## v1: standard containers

Written to be obviously correct.

| Part | Container | Why |
|---|---|---|
| Price levels | `std::map<Price, PriceLevel>`, bids sorted high to low | Any price works; best price is `begin()` |
| Orders at one price | `std::list<Order>` | Removing from the middle is O(1) and doesn't move other orders |
| Find an order by id | `std::unordered_map<OrderId, Locator>` | Cancel is O(1) on average |

The locator stores the order's side, price and list position. It stores the
price rather than a map iterator because a level is erased when it empties,
and a stored iterator to it would then dangle.

`LockedBook` wraps v1 in a single `std::mutex` so the cost of sharing one book
between threads can be measured. Matching is sequential (each order depends on
the book left by the last one), so a lock can only make threads take turns.

## v2: built for speed

Same behavior and events as v1. Every change targets a cost v1 pays on each
operation.

| v1 cost | v2 change |
|---|---|
| A heap allocation for every resting order (list node, map node, hash node) | **Order pool**: all order slots are allocated once at startup; a free list hands them out and takes them back |
| Pointer chasing through tree and list nodes spread across memory | **Intrusive queue**: each slot stores the slot numbers of the orders before and after it, inside one array |
| Tree search to find a price level | **Flat price array**: one entry per price tick, so a level is an array index |
| Hashing the id on every cancel | **Direct id table**: an array indexed by order id gives the slot |
| A new `std::vector` of events per call | **Caller-owned event buffer**: events are appended to a vector the caller reuses |

The trade-off is fixed limits chosen up front in `v2::Config`: the price range,
the maximum number of resting orders, and the largest order id. Orders outside
them are rejected rather than silently mishandled. The best price is tracked
directly; when the best level empties, v2 walks to the next non-empty level.

A test replaces global `operator new` and runs 200,000 operations to prove v2
makes no allocations once running.

## Real market data

`itch_replay` rebuilds a real exchange's book for one stock from Nasdaq
TotalView-ITCH 5.0 data, then replays it through v1 and v2.

- **Source**: Nasdaq's free sample files at https://emi.nasdaq.com/ITCH/
  (full trading days for Nasdaq, BX and PSX). Smaller PSX files are a good
  start, for example `Nasdaq PSX ITCH/20190130.PSX_ITCH_50.gz` (about 540 MB).
- **Format**: binary messages, each with a 2-byte length. Fields are big-endian;
  prices have 4 implied decimals. Spec:
  https://www.nasdaqtrader.com/content/technicalsupport/specifications/dataproducts/NQTVITCHspecification.pdf
- **Decoder** (`include/itch/itch.hpp`): reads fields byte by byte (no casting
  the buffer to a struct), checks each message's length, and returns
  `std::expected<Message, DecodeError>`. Unused message types are skipped.
- **Replay builder** (`include/itch/replay.hpp`): keeps one stock's messages and
  turns them into book operations. Add becomes `add`, a partial execution or
  cancel becomes a size reduction, delete becomes `cancel`, replace becomes
  cancel + add. ITCH order numbers are large, so each order gets a small id.

ITCH is the exchange's output, so its adds rest and never cross. A correct
rebuild therefore shows zero trades, and v1 and v2 must produce identical
events on every message. The tool reports both.

## How correctness is checked

| Check | What it proves |
|---|---|
| Behavior tests on both engines | Fills, partial fills, multi-level crosses, time priority, cancels, modifies, rejects |
| Random operations vs. a brute-force reference book | Same events as a slow, obviously correct implementation (5 seeds x 5,000 ops, each engine) |
| v2 vs. v1 on random streams | Identical events (20 seeds x 20,000 ops) |
| Determinism | Same input, same events, every run |
| No allocation in steady state | v2 makes zero heap allocations over 200,000 ops |
| ITCH decoder tests | Each message type built from hand-written bytes; truncated and unknown input handled |
| Threaded test | Concurrent adds through `LockedBook` (run under ThreadSanitizer) |
| Sanitizers | Every test runs under AddressSanitizer + UndefinedBehaviorSanitizer, and under ThreadSanitizer |

## Build, test, run

Requires CMake 3.20+, Ninja, and a C++23 compiler (built and tested with GCC 15.2).

Tests, with sanitizers:

    cmake -S . -B build-asan -G Ninja -DCMAKE_BUILD_TYPE=Debug -DSANITIZE=address,undefined
    cmake --build build-asan && ctest --test-dir build-asan --output-on-failure

    cmake -S . -B build-tsan -G Ninja -DCMAKE_BUILD_TYPE=Debug -DSANITIZE=thread
    cmake --build build-tsan && ctest --test-dir build-tsan --output-on-failure

Benchmark (Release only):

    cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release
    cmake --build build-release
    ./build-release/orderbook_bench

Real data:

    gunzip -k 20190130.PSX_ITCH_50.gz
    ./build-release/itch_replay 20190130.PSX_ITCH_50 --top      # most active stocks
    ./build-release/itch_replay 20190130.PSX_ITCH_50 AAPL 3     # replay one, 3 runs

## Results

All numbers are **VM numbers**: OrbStack Ubuntu VM on Apple Silicon (aarch64,
8 CPUs), GCC 15.2, Release build (`-O3 -DNDEBUG`). Each figure is the range
over 3 runs. The VM clock ticks every 41 ns, so per-operation latencies are
rounded to it: 42 ns means "one tick or less". Throughput is not affected.

### Synthetic workload (`orderbook_bench`)

100,000 resting orders held steady; 1,000,000 operations: about 50% adds,
40% cancels of live orders, 10% market orders. Seed 42.

| Engine | Throughput | Time per operation |
|---|---|---|
| v1 | 8.30M to 8.86M ops/s | 113 to 121 ns |
| v2 | 49.8M to 61.5M ops/s | 16 to 20 ns |
| **v2 vs v1** | **5.6x to 7.0x** | |

Latency per operation, nanoseconds:

| Engine | Operation | p50 | p99 | p99.9 |
|---|---|---|---|---|
| v1 | add | 125 | 167 to 208 | 292 to 2,000 |
| v1 | cancel | 208 to 250 | 417 to 501 | 4,000 to 4,334 |
| v1 | match | 83 | 292 to 333 | 458 to 2,167 |
| v2 | add | 42 | 83 to 84 | 125 to 167 |
| v2 | cancel | 42 to 83 | 209 to 292 | 2,542 to 3,666 |
| v2 | match | 42 | 125 | 209 to 250 |

One mutex around v1, shared by N threads (same total work):

| Setup | Throughput | vs. no lock |
|---|---|---|
| No lock, 1 thread | 7.09M to 9.60M ops/s | |
| Mutex, 1 thread | 7.46M to 7.79M ops/s | 0.81x to 1.10x |
| Mutex, 2 threads | 3.13M to 3.39M ops/s | 0.33x to 0.46x |
| Mutex, 4 threads | 2.92M to 3.17M ops/s | 0.33x to 0.44x |

### Real market data (`itch_replay`)

Nasdaq PSX, full trading day `20190130.PSX_ITCH_50` (1.24 GB, 41.4M messages).

Correctness, for both symbols: v1 and v2 produced identical events on every
operation, with 0 trades, 0 rejects, 0 unknown order references, and the book
never crossed.

| Symbol | Book operations | Peak resting orders | v1 | v2 | v2 vs v1 |
|---|---|---|---|---|---|
| QQQ | 979,275 | 133 | 15.4M to 15.9M ops/s | 70.4M to 72.5M ops/s | 4.5x to 4.7x |
| MSFT | 322,757 | 38 | 14.9M to 15.1M ops/s | 68.4M to 70.0M ops/s | 4.5x to 4.7x |

QQQ latency, nanoseconds (p50 / p99.9): v1 add 84 / 167 to 375, cancel 83 / 167
to 250; v2 add 42 / 84 to 125, cancel 42 / 333.

Parsing the whole file: 11.7M messages/s when read from disk, 45.0M
messages/s when the file was already in memory (second run), including the
work of building the replay.

### What the numbers show

- **v2 is 5.6x to 7.0x faster on a deep book and 4.5x to 4.7x on real data.**
  The real PSX books are tiny (38 to 133 resting orders), so v1's trees and
  lists already sit in cache there and its disadvantage is smaller.
- **v1.0.0's worst-case add spikes (9 to 12 ms) disappeared** once the
  benchmark held the book at a steady size. That fits hash-table resizing
  during growth. Pre-sizing the index (`reserve`) did not clearly change the
  tail in steady state, so the cause is likely but not proven.
- **Cancels have the longest tail in both engines** (p99.9 of 2.5 to 4.6 µs).
  A likely cause is cache misses from cancelling orders at random places in
  memory; this is the next thing to measure.
- **Sharing one mutex costs little with one thread and a lot with two or
  more**: throughput falls to a third to a half. Matching stays on one thread.

## Repository layout

    include/orderbook/   v1 book, price level, events, types, LockedBook
    include/orderbook/v2 v2 book
    include/itch/        ITCH decoder and replay builder
    src/                 implementations
    tests/               GoogleTest suites (engines, ITCH, allocation)
    bench/               synthetic benchmark
    tools/               itch_replay
    docs/design.md       design notes and costs
