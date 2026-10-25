# orderbook

A limit order book for one instrument, written in C++23.
Orders match by price, then by arrival time (price-time priority).

## What it does

- Accepts limit and market orders, cancels, and modifies
- Matches the best price first; at the same price, the earliest order first
- Trades at the resting order's price
- Keeps an order's place in line when its size is reduced; a price change or
  a size increase sends it to the back
- Never leaves the book crossed (best bid always below best ask)

## How it works

| Part | Choice | Why |
|---|---|---|
| Price levels | `std::map`, sorted best-first | Works for any price range; best price is `begin()` |
| Orders at a price | `std::list` (FIFO) | Removing from the middle is O(1) and leaves other orders' positions valid |
| Order lookup | `std::unordered_map` by id | Cancel finds an order in O(1) |
| Results | `std::variant` events | One return type for accepts, trades, cancels, modifies, rejects |
| Thread-safe version | `LockedBook`, one `std::mutex` | Used to measure what locking costs |

Design notes and costs: [docs/design.md](docs/design.md)

## Build and test

```
cmake -S . -B build-asan -G Ninja -DCMAKE_BUILD_TYPE=Debug -DSANITIZE=address,undefined
cmake --build build-asan && ctest --test-dir build-asan --output-on-failure
```

For data races, repeat with `build-tsan` and `-DSANITIZE=thread`.

## Benchmark

```
cmake -S . -B build-release -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build-release
./build-release/orderbook_bench
```

## Results

### Correctness

| Check | Result |
|---|---|
| Unit and property tests (ASan + UBSan) | 25 / 25 pass |
| Random operations vs. a brute-force reference book | Identical events, 5 seeds x 5,000 ops |
| Replay of the same input twice | Identical events |

Machine: OrbStack Ubuntu VM on Apple Silicon (aarch64), GCC 15.2, Debug build.

### Performance

Not measured yet. Numbers will be added only with machine, compiler, flags
and method. Numbers from the VM will be labeled "VM numbers".
