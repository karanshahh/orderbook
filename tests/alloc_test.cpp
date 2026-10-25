// Proves v2 does not allocate in steady state.
// This file replaces global operator new/delete to count allocations, so it
// is its own test executable (one replacement per program). Every form of
// new/delete is replaced, so all of them pair malloc with free.

#include <gtest/gtest.h>

#include <atomic>
#include <cstdlib>
#include <new>
#include <random>
#include <vector>

#include "orderbook/v2/book.hpp"

namespace {
std::atomic<bool>        g_counting{false};
std::atomic<std::size_t> g_allocs{0};

void* counted_alloc(std::size_t n) noexcept {
    if (g_counting.load(std::memory_order_relaxed)) g_allocs.fetch_add(1, std::memory_order_relaxed);
    return std::malloc(n == 0 ? 1 : n);
}
}  // namespace

void* operator new(std::size_t n) {
    if (void* p = counted_alloc(n)) return p;
    throw std::bad_alloc{};
}
void* operator new[](std::size_t n) {
    if (void* p = counted_alloc(n)) return p;
    throw std::bad_alloc{};
}
void* operator new(std::size_t n, const std::nothrow_t&) noexcept   { return counted_alloc(n); }
void* operator new[](std::size_t n, const std::nothrow_t&) noexcept { return counted_alloc(n); }

void operator delete(void* p) noexcept                              { std::free(p); }
void operator delete[](void* p) noexcept                            { std::free(p); }
void operator delete(void* p, std::size_t) noexcept                 { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept               { std::free(p); }
void operator delete(void* p, const std::nothrow_t&) noexcept       { std::free(p); }
void operator delete[](void* p, const std::nothrow_t&) noexcept     { std::free(p); }

using namespace ob;

TEST(Allocation, V2SteadyStateAllocatesNothing) {
    v2::Book book{v2::Config{Price{0}, Price{2'000}, 100'000, 1'000'000}};
    std::vector<Event> out;
    out.reserve(4'096);

    // Pre-generate the whole workload so generating it is not counted.
    struct Op { int kind; Order o; };
    std::mt19937_64 rng(7);
    std::uniform_int_distribution<int> kind(0, 9), px(900, 1'100), qty(1, 50), side(0, 1);
    std::vector<Op> ops;
    ops.reserve(200'000);
    std::uint64_t next = 1;
    for (int i = 0; i < 200'000; ++i) {
        int k = kind(rng);
        Side s = side(rng) ? Side::Buy : Side::Sell;
        if (k < 6) {
            ops.push_back({0, Order{OrderId{next++}, s, OrderType::Limit, Price{px(rng)},
                                    Quantity{static_cast<std::uint64_t>(qty(rng))}}});
        } else if (k < 9) {
            std::uniform_int_distribution<std::uint64_t> pick(1, next);
            ops.push_back({1, Order{OrderId{pick(rng)}, s, OrderType::Limit, Price{0}, Quantity{0}}});
        } else {
            ops.push_back({0, Order{OrderId{next++}, s, OrderType::Market, Price{0},
                                    Quantity{static_cast<std::uint64_t>(qty(rng))}}});
        }
    }

    g_allocs = 0;
    g_counting = true;
    for (const auto& op : ops) {
        out.clear();
        if (op.kind == 0) book.add(op.o, out);
        else              book.cancel(op.o.id, out);
    }
    g_counting = false;

    EXPECT_EQ(g_allocs.load(), 0u);
    EXPECT_FALSE(book.crossed());
}
