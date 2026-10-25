// Replays real Nasdaq ITCH 5.0 data for one symbol through v1 and v2.
//
//   itch_replay <file> --top          list the 15 most active symbols
//   itch_replay <file> <SYMBOL> [runs] replay that symbol (default 3 runs)
//
// The file must be uncompressed (gunzip first).

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

#include "itch/itch.hpp"
#include "itch/replay.hpp"
#include "orderbook/book.hpp"
#include "orderbook/v2/book.hpp"

using Clock = std::chrono::steady_clock;
using itch::ReplayOp;

namespace {

double secs_since(Clock::time_point t0) {
    return std::chrono::duration<double>(Clock::now() - t0).count();
}

int top_symbols(const std::string& path) {
    itch::FileReader in(path);
    if (!in.ok()) { std::fprintf(stderr, "cannot open %s\n", path.c_str()); return 1; }
    std::map<std::uint16_t, std::string> names;
    std::map<std::uint16_t, std::uint64_t> adds;
    std::span<const std::byte> msg;
    std::uint64_t n = 0;
    while (in.next(msg)) {
        ++n;
        auto r = itch::decode(msg);
        if (!r) continue;
        if (auto* d = std::get_if<itch::StockDirectory>(&*r)) names[d->locate] = itch::to_string(d->stock);
        else if (auto* a = std::get_if<itch::AddOrder>(&*r)) ++adds[a->locate];
    }
    std::vector<std::pair<std::uint64_t, std::uint16_t>> v;
    for (auto& [loc, c] : adds) v.push_back({c, loc});
    std::sort(v.rbegin(), v.rend());
    std::printf("%llu messages\n%-10s %12s\n", (unsigned long long)n, "symbol", "add orders");
    for (std::size_t i = 0; i < std::min<std::size_t>(15, v.size()); ++i)
        std::printf("%-10s %12llu\n", names[v[i].second].c_str(), (unsigned long long)v[i].first);
    return 0;
}

// ---- engines ----
std::size_t run_v1(ob::Book& b, const ReplayOp& op) {
    switch (op.kind) {
        case ReplayOp::Kind::Add:    return b.add(op.order).size();
        case ReplayOp::Kind::Cancel: return b.cancel(op.order.id).size();
        case ReplayOp::Kind::Reduce: return b.modify(op.order.id, op.order.price, op.order.remaining).size();
    }
    return 0;
}
std::size_t run_v2(ob::v2::Book& b, std::vector<ob::Event>& out, const ReplayOp& op) {
    out.clear();
    switch (op.kind) {
        case ReplayOp::Kind::Add:    b.add(op.order, out); break;
        case ReplayOp::Kind::Cancel: b.cancel(op.order.id, out); break;
        case ReplayOp::Kind::Reduce: b.modify(op.order.id, op.order.price, op.order.remaining, out); break;
    }
    return out.size();
}

volatile std::size_t g_sink = 0;

struct Pcts { std::uint64_t p50, p99, p999, max; };
Pcts pcts(std::vector<std::uint64_t>& v) {
    if (v.empty()) return {0, 0, 0, 0};
    std::sort(v.begin(), v.end());
    auto at = [&](double p) { return v[static_cast<std::size_t>(p * static_cast<double>(v.size() - 1))]; };
    return {at(0.50), at(0.99), at(0.999), v.back()};
}

}  // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <file> --top | <SYMBOL> [runs]\n", argv[0]);
        return 1;
    }
    const std::string path = argv[1];
    if (std::strcmp(argv[2], "--top") == 0) return top_symbols(path);
    const std::string symbol = argv[2];
    const int runs = argc > 3 ? std::stoi(argv[3]) : 3;

    // ---- 1) parse ----
    itch::FileReader in(path);
    if (!in.ok()) { std::fprintf(stderr, "cannot open %s\n", path.c_str()); return 1; }
    itch::ReplayBuilder rb(symbol);
    std::span<const std::byte> msg;
    std::uint64_t bytes = 0;
    auto t0 = Clock::now();
    while (in.next(msg)) {
        bytes += msg.size() + 2;
        auto r = itch::decode(msg);
        rb.count(r);
        if (r) rb.feed(*r);
    }
    const double parse_s = secs_since(t0);
    if (!rb.found_symbol()) { std::fprintf(stderr, "symbol %s not in stock directory\n", symbol.c_str()); return 1; }

    const auto& st = rb.stats();
    std::printf("file         %s (%.2f GB)\n", path.c_str(), static_cast<double>(bytes) / 1e9);
    std::printf("parse        %llu messages in %.2f s = %.1f M msg/s (includes disk reads)\n",
                (unsigned long long)st.messages, parse_s, static_cast<double>(st.messages) / parse_s / 1e6);
    std::printf("             decoded %llu, other types %llu, truncated %llu%s\n",
                (unsigned long long)st.decoded, (unsigned long long)st.ignored,
                (unsigned long long)st.truncated, in.truncated_tail() ? ", file ends mid-message" : "");
    std::printf("symbol       %s: %llu adds, %llu executions, %llu cancels, %llu deletes, %llu replaces, unknown refs %llu\n",
                symbol.c_str(), (unsigned long long)st.adds, (unsigned long long)st.executions,
                (unsigned long long)st.cancels, (unsigned long long)st.deletes, (unsigned long long)st.replaces,
                (unsigned long long)st.unknown_refs);

    std::vector<ReplayOp> ops = rb.ops();
    if (ops.empty()) return 0;

    // Tick size: stocks above $1 trade in whole cents, so prices in ITCH's
    // $0.0001 units are usually multiples of 100. Use cents if so: 100x fewer levels.
    bool cents = true;
    for (const auto& op : ops) cents = cents && op.order.price.ticks % 100 == 0;
    const std::int64_t unit = cents ? 100 : 1;
    if (cents) for (auto& op : ops) op.order.price.ticks /= 100;

    // v2 needs one array slot per tick between the lowest and highest price.
    // A few far-away orders (a $1 bid on a $150 stock) would make that huge,
    // so if the range is too wide, orders outside the middle 99.8% of add
    // prices are dropped from the replay for both engines (count reported).
    constexpr std::int64_t kMaxLevels = 2'000'000;
    std::vector<std::int64_t> prices;
    for (const auto& op : ops) if (op.kind == ReplayOp::Kind::Add) prices.push_back(op.order.price.ticks);
    std::sort(prices.begin(), prices.end());
    std::int64_t lo = prices.front(), hi = prices.back();
    std::uint64_t dropped_orders = 0;
    if (hi - lo + 1 > kMaxLevels) {
        lo = prices[prices.size() / 1000];
        hi = prices[prices.size() - 1 - prices.size() / 1000];
        std::vector<char> drop(rb.max_id(), 0);
        std::vector<ReplayOp> kept;
        kept.reserve(ops.size());
        for (const auto& op : ops) {
            const auto id = op.order.id.value;
            if (op.kind == ReplayOp::Kind::Add && (op.order.price.ticks < lo || op.order.price.ticks > hi)) {
                drop[id] = 1;
                ++dropped_orders;
            }
            if (!drop[id]) kept.push_back(op);
        }
        ops.swap(kept);
        if (hi - lo + 1 > kMaxLevels) {
            std::fprintf(stderr, "price range still too wide (%lld ticks); pick another symbol\n", (long long)(hi - lo + 1));
            return 1;
        }
    }
    std::printf("book ops     %zu (peak %zu resting orders), tick $%s, price window $%.2f to $%.2f, orders dropped outside window: %llu\n",
                ops.size(), rb.peak_live(), cents ? "0.01" : "0.0001",
                static_cast<double>(lo * unit) / 1e4, static_cast<double>(hi * unit) / 1e4,
                (unsigned long long)dropped_orders);

    const ob::v2::Config cfg{ob::Price{lo}, ob::Price{hi},
                             static_cast<std::uint32_t>(rb.peak_live() + 16), rb.max_id() + 1};

    // ---- 2) correctness: v1 and v2 must agree on every event ----
    {
        ob::Book v1;
        ob::v2::Book v2(cfg);
        std::vector<ob::Event> out;
        std::uint64_t mismatches = 0, trades = 0, rejects = 0, crossed_after = 0;
        for (const auto& op : ops) {
            std::vector<ob::Event> a;
            switch (op.kind) {
                case ReplayOp::Kind::Add:    a = v1.add(op.order); break;
                case ReplayOp::Kind::Cancel: a = v1.cancel(op.order.id); break;
                case ReplayOp::Kind::Reduce: a = v1.modify(op.order.id, op.order.price, op.order.remaining); break;
            }
            run_v2(v2, out, op);
            if (a != out) ++mismatches;
            for (const auto& e : a) {
                trades  += std::holds_alternative<ob::Trade>(e);
                rejects += std::holds_alternative<ob::Rejected>(e);
            }
            crossed_after += v1.crossed();
        }
        std::printf("check        v1 vs v2 mismatched events: %llu; trades: %llu; rejects: %llu; ops leaving book crossed: %llu\n",
                    (unsigned long long)mismatches, (unsigned long long)trades,
                    (unsigned long long)rejects, (unsigned long long)crossed_after);
        std::printf("             (a rebuilt exchange book should show 0 trades: ITCH adds rest, they never cross)\n");
    }

    // ---- 3) performance ----
    std::printf("\nruns: %d   latency in ns, throughput over all %zu ops\n", runs, ops.size());
    std::printf("  %-4s %-3s %-7s %7s %7s %8s %9s %12s\n", "eng", "run", "op", "p50", "p99", "p99.9", "max", "M ops/s");
    for (int r = 1; r <= runs; ++r) {
        for (int eng = 1; eng <= 2; ++eng) {
            std::vector<std::uint64_t> lat[3];
            ob::Book v1;
            ob::v2::Book v2(cfg);
            std::vector<ob::Event> out;
            out.reserve(1'024);

            // timed per op
            for (const auto& op : ops) {
                auto a = Clock::now();
                g_sink = g_sink + (eng == 1 ? run_v1(v1, op) : run_v2(v2, out, op));
                auto b = Clock::now();
                lat[static_cast<int>(op.kind)].push_back(static_cast<std::uint64_t>(
                    std::chrono::duration_cast<std::chrono::nanoseconds>(b - a).count()));
            }
            // throughput on fresh books, no per-op timing
            ob::Book v1b;
            ob::v2::Book v2b(cfg);
            auto t = Clock::now();
            for (const auto& op : ops) g_sink = g_sink + (eng == 1 ? run_v1(v1b, op) : run_v2(v2b, out, op));
            const double mops = static_cast<double>(ops.size()) / secs_since(t) / 1e6;

            const char* names[3] = {"add", "cancel", "reduce"};
            for (int k = 0; k < 3; ++k) {
                Pcts p = pcts(lat[k]);
                std::printf("  %-4s %-3d %-7s %7llu %7llu %8llu %9llu %12s\n", eng == 1 ? "v1" : "v2", r, names[k],
                            (unsigned long long)p.p50, (unsigned long long)p.p99,
                            (unsigned long long)p.p999, (unsigned long long)p.max,
                            k == 0 ? std::to_string(mops).substr(0, 6).c_str() : "");
            }
        }
    }
    return 0;
}
