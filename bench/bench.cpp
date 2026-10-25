// Synthetic benchmark: v1 vs v2 latency and throughput, plus lock contention.
// Build in Release without sanitizers; the program warns if you don't.

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
#include "orderbook/v2/book.hpp"

using namespace ob;
using Clock = std::chrono::steady_clock;

namespace {

// ---------------- workload ----------------
// Steady state: the book is held near a target size. Passive limit orders rest
// within 50 ticks of the mid (buys below, sells above) and never trade. Market
// orders always trade. Cancels always hit a live order: a shadow v1 book is run
// while generating, so we know which ids are still resting.
enum class Kind : std::uint8_t { Add, Cancel, Match };
struct Op { Kind kind; Order order; };

constexpr std::int64_t kMid = 10'000;
constexpr int kPreload = 100'000;

struct Workload {
    std::vector<Op> preload;   // run untimed before measuring
    std::vector<Op> ops;       // timed
    std::uint64_t   max_id = 0;
};

Workload make_workload(int n_ops, std::uint64_t seed) {
    std::mt19937_64 rng(seed);
    std::uniform_int_distribution<int> pct(0, 99), offset(1, 50), qty(1, 100), side(0, 1);
    Book shadow;
    std::vector<std::uint64_t> live;   // candidate ids; dead ones removed lazily
    std::uint64_t next = 1;
    Workload w;

    auto passive = [&] {
        Side s = side(rng) ? Side::Buy : Side::Sell;
        std::int64_t px = s == Side::Buy ? kMid - offset(rng) : kMid + offset(rng);
        Order o{OrderId{next++}, s, OrderType::Limit, Price{px},
                Quantity{static_cast<std::uint64_t>(qty(rng))}};
        live.push_back(o.id.value);
        return Op{Kind::Add, o};
    };

    for (int i = 0; i < kPreload; ++i) {
        w.preload.push_back(passive());
        shadow.add(w.preload.back().order);
    }

    while (static_cast<int>(w.ops.size()) < n_ops) {
        int p = pct(rng);
        const std::size_t size = shadow.open_orders();
        Op op;
        if (size < kPreload * 9 / 10) p = 0;              // too small: add
        else if (size > kPreload * 11 / 10) p = 55;       // too big: cancel
        if (p < 50) {
            op = passive();
        } else if (p < 90) {
            std::uint64_t id = 0;
            while (!live.empty()) {
                std::uniform_int_distribution<std::size_t> pick(0, live.size() - 1);
                std::size_t i = pick(rng);
                if (shadow.find(OrderId{live[i]})) { id = live[i]; live[i] = live.back(); live.pop_back(); break; }
                live[i] = live.back(); live.pop_back();     // filled earlier: drop it
            }
            if (id == 0) continue;
            op = Op{Kind::Cancel, Order{OrderId{id}, Side::Buy, OrderType::Limit, Price{0}, Quantity{0}}};
        } else {
            Side s = side(rng) ? Side::Buy : Side::Sell;
            op = Op{Kind::Match, Order{OrderId{next++}, s, OrderType::Market, Price{0},
                                      Quantity{static_cast<std::uint64_t>(qty(rng) / 4 + 1)}}};
        }
        if (op.kind == Kind::Cancel) shadow.cancel(op.order.id);
        else                         shadow.add(op.order);
        w.ops.push_back(op);
    }
    w.max_id = next + 1;
    return w;
}

v2::Config v2_config(std::uint64_t max_id) {
    return v2::Config{Price{kMid - 1'000}, Price{kMid + 1'000}, 400'000, max_id};
}

// ---------------- engines behind one interface ----------------
struct V1      { Book b;              std::size_t run(const Op& op); };
struct V1Res   { Book b;              std::size_t run(const Op& op); };   // index pre-sized
struct V2      { v2::Book b; std::vector<Event> out;
                 explicit V2(std::uint64_t max_id) : b(v2_config(max_id)) { out.reserve(4'096); }
                 std::size_t run(const Op& op); };

std::size_t V1::run(const Op& op) {
    return op.kind == Kind::Cancel ? b.cancel(op.order.id).size() : b.add(op.order).size();
}
std::size_t V1Res::run(const Op& op) {
    return op.kind == Kind::Cancel ? b.cancel(op.order.id).size() : b.add(op.order).size();
}
std::size_t V2::run(const Op& op) {
    out.clear();
    if (op.kind == Kind::Cancel) b.cancel(op.order.id, out);
    else                         b.add(op.order, out);
    return out.size();
}

volatile std::size_t g_sink = 0;   // keeps the compiler from deleting the work

std::uint64_t ns_between(Clock::time_point a, Clock::time_point b) {
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count());
}

// ---------------- reporting ----------------
struct Pcts { std::uint64_t p50, p90, p99, p999, max; };
Pcts pcts(std::vector<std::uint64_t>& v) {
    std::sort(v.begin(), v.end());
    auto at = [&](double p) { return v[static_cast<std::size_t>(p * static_cast<double>(v.size() - 1))]; };
    return {at(0.50), at(0.90), at(0.99), at(0.999), v.back()};
}
void row(const char* engine, const char* op, std::vector<std::uint64_t>& v) {
    if (v.empty()) return;
    Pcts p = pcts(v);
    std::printf("  %-10s %-7s %9zu %7llu %7llu %7llu %8llu %10llu\n", engine, op, v.size(),
                (unsigned long long)p.p50, (unsigned long long)p.p90, (unsigned long long)p.p99,
                (unsigned long long)p.p999, (unsigned long long)p.max);
}

template <typename E>
void latency(const char* name, E& e, const Workload& w) {
    for (const auto& op : w.preload) g_sink = g_sink + e.run(op);
    std::vector<std::uint64_t> add, cancel, match;
    add.reserve(w.ops.size()); cancel.reserve(w.ops.size()); match.reserve(w.ops.size());
    for (const auto& op : w.ops) {
        auto t0 = Clock::now();
        g_sink = g_sink + e.run(op);
        auto t1 = Clock::now();
        (op.kind == Kind::Add ? add : op.kind == Kind::Cancel ? cancel : match).push_back(ns_between(t0, t1));
    }
    row(name, "add", add);
    row(name, "cancel", cancel);
    row(name, "match", match);
}

template <typename E>
double throughput(E& e, const Workload& w) {
    for (const auto& op : w.preload) g_sink = g_sink + e.run(op);
    auto t0 = Clock::now();
    for (const auto& op : w.ops) g_sink = g_sink + e.run(op);
    return static_cast<double>(w.ops.size()) / std::chrono::duration<double>(Clock::now() - t0).count();
}

std::uint64_t clock_tick_ns() {
    // Smallest non-zero step the clock reports.
    std::uint64_t best = ~0ull;
    for (int i = 0; i < 200'000; ++i) {
        auto a = Clock::now(), b = Clock::now();
        while (b == a) b = Clock::now();
        best = std::min(best, ns_between(a, b));
    }
    return best;
}

void print_build_info() {
#if defined(__clang__)
    std::printf("compiler     clang %s\n", __clang_version__);
#elif defined(__GNUC__)
    std::printf("compiler     gcc %s\n", __VERSION__);
#endif
#ifdef NDEBUG
    std::printf("build        Release (NDEBUG on)\n");
#else
    std::printf("build        NOT Release: numbers are not meaningful\n");
#endif
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
    std::printf("WARNING      sanitizer build: numbers are not meaningful\n");
#endif
    std::printf("hw threads   %u\n", std::thread::hardware_concurrency());
}

}  // namespace

int main(int argc, char** argv) {
    const int n_ops = argc > 1 ? std::stoi(argv[1]) : 1'000'000;
    const std::uint64_t seed = 42;

    print_build_info();
    std::printf("clock tick   %llu ns (per-op latencies are rounded to this)\n",
                (unsigned long long)clock_tick_ns());

    const Workload w = make_workload(n_ops, seed);
    std::printf("workload     %d preloaded orders, %zu timed ops, seed %llu\n",
                kPreload, w.ops.size(), (unsigned long long)seed);
    std::printf("             ~50%% add, ~40%% cancel (live orders only), ~10%% market; book held near %d orders\n\n", kPreload);

    // 1) Latency per operation
    std::printf("[1] latency per operation, ns\n");
    std::printf("  %-10s %-7s %9s %7s %7s %7s %8s %10s\n", "engine", "op", "count", "p50", "p90", "p99", "p99.9", "max");
    { V1 e;                         latency("v1", e, w); }
    { V1Res e; e.b.reserve(400'000); latency("v1+reserve", e, w); }
    { V2 e(w.max_id);               latency("v2", e, w); }

    // 2) Throughput
    std::printf("\n[2] throughput, single thread\n");
    double t1, t2;
    { V1 e;           t1 = throughput(e, w); }
    { V2 e(w.max_id); t2 = throughput(e, w); }
    std::printf("  v1   %10.2f M ops/s   %6.1f ns/op\n", t1 / 1e6, 1e9 / t1);
    std::printf("  v2   %10.2f M ops/s   %6.1f ns/op   (%.2fx v1)\n", t2 / 1e6, 1e9 / t2, t2 / t1);

    // 3) Locking: same per-thread op slices for every row
    std::printf("\n[3] one mutex around v1, N threads sharing it (total ops fixed)\n");
    double base = 0;
    {
        const Workload slice = make_workload(n_ops, seed + 1);
        V1 e;
        base = throughput(e, slice);
        std::printf("  no lock, 1 thread   %8.2f M ops/s\n", base / 1e6);
    }
    for (unsigned n : {1u, 2u, 4u}) {
        std::vector<Workload> slices;
        for (unsigned t = 0; t < n; ++t) {
            Workload s = make_workload(n_ops / static_cast<int>(n), seed + 1 + t);
            const std::uint64_t shift = (t + 1) * 100'000'000ull;   // disjoint ids per thread
            for (auto* list : {&s.preload, &s.ops})
                for (auto& op : *list) op.order.id.value += shift;
            slices.push_back(std::move(s));
        }
        LockedBook book;
        for (const auto& s : slices)
            for (const auto& op : s.preload) book.add(op.order);

        std::vector<std::size_t> sinks(n, 0);
        auto t0 = Clock::now();
        std::vector<std::thread> ts;
        for (unsigned t = 0; t < n; ++t)
            ts.emplace_back([&book, &s = slices[t], &out = sinks[t]] {
                std::size_t local = 0;
                for (const auto& op : s.ops)
                    local += op.kind == Kind::Cancel ? book.cancel(op.order.id).size()
                                                     : book.add(op.order).size();
                out = local;
            });
        for (auto& th : ts) th.join();
        const double secs = std::chrono::duration<double>(Clock::now() - t0).count();
        for (auto v : sinks) g_sink = g_sink + v;
        std::size_t total = 0;
        for (const auto& s : slices) total += s.ops.size();
        const double r = static_cast<double>(total) / secs;
        std::printf("  mutex, %u thread(s)  %8.2f M ops/s   (%.2fx no lock)\n", n, r / 1e6, r / base);
    }
    return 0;
}
