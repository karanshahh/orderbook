// Order book benchmark: per-operation latency percentiles and throughput,
// plus the cost of wrapping the book in a mutex.
//
// Build in Release (no sanitizers) or the numbers mean nothing.
// Numbers from the OrbStack VM are "VM numbers".

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "orderbook/book.hpp"
#include "orderbook/locked_book.hpp"

using namespace ob;
using Clock = std::chrono::steady_clock;

namespace {

// ---------------- workload ----------------
// Passive limit orders rest away from the mid (buys below, sells above), so
// they never trade. Market orders always trade. Cancels hit a random id that
// was added earlier (it may already be filled, which is a cheap reject).
enum class Kind : std::uint8_t { AddPassive, Cancel, Match };

struct Op {
    Kind kind;
    Order order;   // AddPassive / Match
    OrderId id;    // Cancel
};

struct Config {
    int ops = 1'000'000;
    int warmup_orders = 100'000;
    int add_pct = 60, cancel_pct = 30;   // remainder = market (match)
    std::uint64_t seed = 42;
};

std::vector<Op> make_ops(const Config& c, std::uint64_t id_base, std::uint64_t seed) {
    std::mt19937_64 rng(seed);
    std::uniform_int_distribution<int> pct(0, 99), offset(1, 50), qty(1, 100), side(0, 1);
    constexpr std::int64_t mid = 10'000;
    std::vector<Op> ops;
    ops.reserve(static_cast<std::size_t>(c.ops));
    std::uint64_t next = id_base;
    for (int i = 0; i < c.ops; ++i) {
        int p = pct(rng);
        Side s = side(rng) ? Side::Buy : Side::Sell;
        if (p < c.add_pct || next == id_base) {
            std::int64_t px = s == Side::Buy ? mid - offset(rng) : mid + offset(rng);
            ops.push_back({Kind::AddPassive,
                           Order{OrderId{next++}, s, OrderType::Limit, Price{px},
                                 Quantity{static_cast<std::uint64_t>(qty(rng))}},
                           {}});
        } else if (p < c.add_pct + c.cancel_pct) {
            std::uniform_int_distribution<std::uint64_t> pick(id_base, next - 1);
            ops.push_back({Kind::Cancel, {}, OrderId{pick(rng)}});
        } else {
            ops.push_back({Kind::Match,
                           Order{OrderId{next++}, s, OrderType::Market, Price{0},
                                 Quantity{static_cast<std::uint64_t>(qty(rng) / 4 + 1)}},
                           {}});
        }
    }
    return ops;
}

template <typename B>
void prefill(B& book, int n, std::uint64_t id_base, std::uint64_t seed) {
    std::mt19937_64 rng(seed);
    std::uniform_int_distribution<int> offset(1, 50), qty(1, 100), side(0, 1);
    for (int i = 0; i < n; ++i) {
        Side s = side(rng) ? Side::Buy : Side::Sell;
        std::int64_t px = s == Side::Buy ? 10'000 - offset(rng) : 10'000 + offset(rng);
        book.add(Order{OrderId{id_base + static_cast<std::uint64_t>(i)}, s, OrderType::Limit,
                       Price{px}, Quantity{static_cast<std::uint64_t>(qty(rng))}});
    }
}

template <typename B>
std::size_t run(B& book, const Op& op) {
    switch (op.kind) {
        case Kind::AddPassive:
        case Kind::Match:  return book.add(op.order).size();
        case Kind::Cancel: return book.cancel(op.id).size();
    }
    return 0;
}

// Keeps the compiler from deleting work whose result is unused.
volatile std::size_t g_sink = 0;

// ---------------- reporting ----------------
void report(const char* name, std::vector<std::uint64_t>& ns) {
    if (ns.empty()) return;
    std::sort(ns.begin(), ns.end());
    auto pct = [&](double p) {
        std::size_t i = static_cast<std::size_t>(p * static_cast<double>(ns.size() - 1));
        return ns[i];
    };
    std::printf("  %-12s n=%-8zu p50=%-6llu p90=%-6llu p99=%-6llu p99.9=%-7llu max=%llu  (ns)\n",
                name, ns.size(),
                (unsigned long long)pct(0.50), (unsigned long long)pct(0.90),
                (unsigned long long)pct(0.99), (unsigned long long)pct(0.999),
                (unsigned long long)ns.back());
}

std::uint64_t clock_overhead_ns() {
    std::vector<std::uint64_t> d(100'000);
    for (auto& x : d) {
        auto a = Clock::now();
        auto b = Clock::now();
        x = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count());
    }
    std::sort(d.begin(), d.end());
    return d[d.size() / 2];
}

void print_build_info() {
    std::printf("== build ==\n");
#if defined(__clang__)
    std::printf("  compiler: clang %s\n", __clang_version__);
#elif defined(__GNUC__)
    std::printf("  compiler: gcc %s\n", __VERSION__);
#endif
#ifdef NDEBUG
    std::printf("  NDEBUG: on\n");
#else
    std::printf("  NDEBUG: OFF  <-- not a Release build, numbers are not meaningful\n");
#endif
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
    std::printf("  SANITIZER BUILD  <-- numbers are not meaningful\n");
#endif
#if defined(__has_feature)
#  if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
    std::printf("  SANITIZER BUILD  <-- numbers are not meaningful\n");
#  endif
#endif
    std::printf("  hardware threads: %u\n", std::thread::hardware_concurrency());
    std::printf("  (record CPU model with: lscpu | head -20)\n");
}

double seconds_since(Clock::time_point t0) {
    return std::chrono::duration<double>(Clock::now() - t0).count();
}

}  // namespace

int main(int argc, char** argv) {
    Config cfg;
    if (argc > 1) cfg.ops = std::stoi(argv[1]);

    print_build_info();
    std::printf("== workload ==\n  ops=%d  warmup resting orders=%d  mix: %d%% add, %d%% cancel, %d%% market  seed=%llu\n",
                cfg.ops, cfg.warmup_orders, cfg.add_pct, cfg.cancel_pct,
                100 - cfg.add_pct - cfg.cancel_pct, (unsigned long long)cfg.seed);
    std::printf("  clock overhead (median of now()-now()): %llu ns, included in every latency below\n",
                (unsigned long long)clock_overhead_ns());

    constexpr std::uint64_t kWarmBase = 1;
    constexpr std::uint64_t kOpsBase = 1'000'000'000ULL;
    const auto ops = make_ops(cfg, kOpsBase, cfg.seed);

    // 1) Per-operation latency, single-threaded Book.
    {
        Book book;
        prefill(book, cfg.warmup_orders, kWarmBase, cfg.seed + 1);
        std::vector<std::uint64_t> add_ns, cancel_ns, match_ns;
        add_ns.reserve(ops.size()); cancel_ns.reserve(ops.size()); match_ns.reserve(ops.size());
        for (const auto& op : ops) {
            auto t0 = Clock::now();
            g_sink = g_sink + run(book, op);
            auto t1 = Clock::now();
            auto ns = static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
            (op.kind == Kind::AddPassive ? add_ns : op.kind == Kind::Cancel ? cancel_ns : match_ns).push_back(ns);
        }
        std::printf("== 1) latency per operation: Book, 1 thread ==\n");
        report("add", add_ns);
        report("cancel", cancel_ns);
        report("match", match_ns);
        std::printf("  book at end: %zu resting orders\n", book.open_orders());
    }

    // 2) Throughput: same ops, no per-op timing. Book vs LockedBook (1 thread).
    std::printf("== 2) throughput, 1 thread (same op sequence) ==\n");
    double book_ops_s = 0;
    {
        Book book;
        prefill(book, cfg.warmup_orders, kWarmBase, cfg.seed + 1);
        auto t0 = Clock::now();
        for (const auto& op : ops) g_sink = g_sink + run(book, op);
        double s = seconds_since(t0);
        book_ops_s = static_cast<double>(ops.size()) / s;
        std::printf("  Book (no lock):        %10.0f ops/s\n", book_ops_s);
    }
    {
        LockedBook book;
        prefill(book, cfg.warmup_orders, kWarmBase, cfg.seed + 1);
        auto t0 = Clock::now();
        for (const auto& op : ops) g_sink = g_sink + run(book, op);
        double s = seconds_since(t0);
        double r = static_cast<double>(ops.size()) / s;
        std::printf("  LockedBook (mutex):    %10.0f ops/s  (%.2fx of no-lock)\n", r, r / book_ops_s);
    }

    // 3) Contention: N threads share one LockedBook. Total ops = cfg.ops, split evenly.
    std::printf("== 3) throughput, N threads sharing one LockedBook (total ops fixed) ==\n");
    for (unsigned n : {1u, 2u, 4u}) {
        LockedBook book;
        prefill(book, cfg.warmup_orders, kWarmBase, cfg.seed + 1);
        Config per = cfg;
        per.ops = cfg.ops / static_cast<int>(n);
        std::vector<std::vector<Op>> slices;
        for (unsigned t = 0; t < n; ++t)
            slices.push_back(make_ops(per, kOpsBase * (t + 2), cfg.seed + 100 + t));  // disjoint ids

        std::vector<std::size_t> sinks(n, 0);   // one slot per thread: no shared writes
        auto t0 = Clock::now();
        std::vector<std::thread> ts;
        for (unsigned t = 0; t < n; ++t)
            ts.emplace_back([&book, &slice = slices[t], &out = sinks[t]] {
                std::size_t local = 0;
                for (const auto& op : slice) local += run(book, op);
                out = local;
            });
        for (auto& th : ts) th.join();
        double s = seconds_since(t0);
        for (auto v : sinks) g_sink = g_sink + v;
        double total = static_cast<double>(per.ops) * n;
        std::printf("  %u thread(s):           %10.0f ops/s  (%.2fx of single-thread no-lock Book)\n",
                    n, total / s, (total / s) / book_ops_s);
    }

    std::printf("== notes ==\n"
                "  Latency includes timer overhead. Means are not reported on purpose.\n"
                "  Run 3+ times; report the spread, not the best run.\n");
    return 0;
}
