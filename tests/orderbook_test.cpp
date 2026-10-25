#include <gtest/gtest.h>

#include <algorithm>
#include <optional>
#include <random>
#include <thread>
#include <variant>
#include <vector>

#include "orderbook/book.hpp"
#include "orderbook/locked_book.hpp"

using namespace ob;

namespace {

Order limit(std::uint64_t id, Side s, std::int64_t px, std::uint64_t qty) {
    return Order{OrderId{id}, s, OrderType::Limit, Price{px}, Quantity{qty}};
}
Order market(std::uint64_t id, Side s, std::uint64_t qty) {
    return Order{OrderId{id}, s, OrderType::Market, Price{0}, Quantity{qty}};
}

template <typename T>
std::vector<T> only(const std::vector<Event>& evs) {
    std::vector<T> out;
    for (const auto& e : evs)
        if (auto p = std::get_if<T>(&e)) out.push_back(*p);
    return out;
}

// ---------------------------------------------------------------------------
// Brute-force reference: a flat vector, scanned every time. Slow and obvious,
// so it's easy to believe it's correct. The real Book must agree with it.
// ---------------------------------------------------------------------------
class RefBook {
public:
    std::vector<Event> add(Order o) {
        if (o.remaining.value == 0) return {Rejected{o.id, RejectReason::ZeroQuantity}};
        if (find(o.id) != resting_.end()) return {Rejected{o.id, RejectReason::DuplicateId}};
        const Side opp = o.side == Side::Buy ? Side::Sell : Side::Buy;
        if (o.type == OrderType::Market && best(opp) == resting_.end())
            return {Rejected{o.id, RejectReason::NoLiquidity}};

        std::vector<Event> out{Accepted{o.id}};
        while (o.remaining.value > 0) {
            auto b = best(opp);
            if (b == resting_.end()) break;
            bool ok = o.type == OrderType::Market ||
                      (o.side == Side::Buy ? b->o.price <= o.price : b->o.price >= o.price);
            if (!ok) break;
            std::uint64_t q = std::min(o.remaining.value, b->o.remaining.value);
            out.push_back(Trade{b->o.id, o.id, b->o.price, Quantity{q}});
            o.remaining.value -= q;
            b->o.remaining.value -= q;
            if (b->o.remaining.value == 0) resting_.erase(b);
        }
        if (o.remaining.value > 0) {
            if (o.type == OrderType::Limit) resting_.push_back({o, seq_++});
            else out.push_back(Cancelled{o.id, o.remaining});
        }
        return out;
    }

    std::vector<Event> cancel(OrderId id) {
        auto it = find(id);
        if (it == resting_.end()) return {Rejected{id, RejectReason::UnknownId}};
        Quantity rem = it->o.remaining;
        resting_.erase(it);
        return {Cancelled{id, rem}};
    }

    std::vector<Event> modify(OrderId id, Price p, Quantity q) {
        auto it = find(id);
        if (it == resting_.end()) return {Rejected{id, RejectReason::UnknownId}};
        if (q.value == 0) return {Rejected{id, RejectReason::ZeroQuantity}};
        if (p == it->o.price && q <= it->o.remaining) {
            it->o.remaining = q;
            return {Modified{id, p, q}};
        }
        Side s = it->o.side;
        resting_.erase(it);
        return add(Order{id, s, OrderType::Limit, p, q});
    }

    std::optional<Price> best_price(Side s) const {
        std::optional<Price> r;
        for (const auto& e : resting_) {
            if (e.o.side != s) continue;
            if (!r || (s == Side::Buy ? e.o.price > *r : e.o.price < *r)) r = e.o.price;
        }
        return r;
    }
    std::size_t open_orders() const { return resting_.size(); }

private:
    struct Entry { Order o; std::uint64_t seq; };
    std::vector<Entry> resting_;
    std::uint64_t seq_ = 0;

    std::vector<Entry>::iterator find(OrderId id) {
        return std::find_if(resting_.begin(), resting_.end(),
                            [&](const Entry& e) { return e.o.id == id; });
    }
    // Best price for side s; among equal prices, the lowest sequence number.
    std::vector<Entry>::iterator best(Side s) {
        auto bestIt = resting_.end();
        for (auto it = resting_.begin(); it != resting_.end(); ++it) {
            if (it->o.side != s) continue;
            if (bestIt == resting_.end()) { bestIt = it; continue; }
            bool better = s == Side::Buy ? it->o.price > bestIt->o.price
                                         : it->o.price < bestIt->o.price;
            bool tie_earlier = it->o.price == bestIt->o.price && it->seq < bestIt->seq;
            if (better || tie_earlier) bestIt = it;
        }
        return bestIt;
    }
};

// One random operation, applied identically to any book type.
struct Op {
    enum Kind { Add, Cancel, Modify } kind;
    Order order;           // for Add
    OrderId id;            // for Cancel / Modify
    Price price;           // for Modify
    Quantity qty;          // for Modify
};

std::vector<Op> random_ops(std::uint64_t seed, int n) {
    std::mt19937_64 rng(seed);
    std::uniform_int_distribution<int> kind(0, 9), px(95, 105), qty(0, 20), pick(0, 1);
    std::vector<Op> ops;
    std::uint64_t next_id = 1;
    for (int i = 0; i < n; ++i) {
        int k = kind(rng);
        Op op{};
        if (k < 6) {
            op.kind = Op::Add;
            Side s = pick(rng) ? Side::Buy : Side::Sell;
            bool mkt = kind(rng) == 0;
            // Occasionally reuse an old id to exercise DuplicateId.
            std::uint64_t id = (kind(rng) == 0 && next_id > 1)
                ? std::uniform_int_distribution<std::uint64_t>(1, next_id - 1)(rng)
                : next_id++;
            op.order = Order{OrderId{id}, s, mkt ? OrderType::Market : OrderType::Limit,
                             Price{px(rng)}, Quantity{static_cast<std::uint64_t>(qty(rng))}};
        } else {
            op.kind = k < 8 ? Op::Cancel : Op::Modify;
            op.id = OrderId{std::uniform_int_distribution<std::uint64_t>(1, next_id)(rng)};
            op.price = Price{px(rng)};
            op.qty = Quantity{static_cast<std::uint64_t>(qty(rng))};
        }
        ops.push_back(op);
    }
    return ops;
}

template <typename B>
std::vector<Event> apply(B& b, const Op& op) {
    switch (op.kind) {
        case Op::Add:    return b.add(op.order);
        case Op::Cancel: return b.cancel(op.id);
        case Op::Modify: return b.modify(op.id, op.price, op.qty);
    }
    return {};
}

}  // namespace

// ---------- PriceLevel ----------
TEST(PriceLevel, AppendUpdatesTotal) {
    PriceLevel lvl;
    lvl.append(limit(1, Side::Buy, 100, 10));
    lvl.append(limit(2, Side::Buy, 100, 5));
    EXPECT_EQ(lvl.total(), Quantity{15});
    EXPECT_EQ(lvl.size(), 2u);
    EXPECT_EQ(lvl.front().id, OrderId{1});
}

TEST(PriceLevel, EraseMiddleKeepsFifo) {
    PriceLevel lvl;
    lvl.append(limit(1, Side::Buy, 100, 1));
    auto mid = lvl.append(limit(2, Side::Buy, 100, 2));
    lvl.append(limit(3, Side::Buy, 100, 3));
    lvl.erase(mid);
    ASSERT_EQ(lvl.size(), 2u);
    EXPECT_EQ(lvl.orders().front().id, OrderId{1});
    EXPECT_EQ(lvl.orders().back().id, OrderId{3});
    EXPECT_EQ(lvl.total(), Quantity{4});
}

// ---------- add / resting ----------
TEST(BookAdd, RestingLimitBuy) {
    Book book;
    auto events = book.add(limit(1, Side::Buy, 100, 10));
    ASSERT_EQ(events.size(), 1u);
    EXPECT_TRUE(std::holds_alternative<Accepted>(events[0]));
    EXPECT_EQ(book.open_orders(), 1u);
    EXPECT_FALSE(book.crossed());
}

TEST(BookAdd, BestBidAndAsk) {
    Book book;
    EXPECT_FALSE(book.best_bid().has_value());
    EXPECT_FALSE(book.best_ask().has_value());
    book.add(limit(1, Side::Buy, 99, 1));
    book.add(limit(2, Side::Buy, 98, 1));
    book.add(limit(3, Side::Sell, 101, 1));
    book.add(limit(4, Side::Sell, 102, 1));
    EXPECT_EQ(book.best_bid(), Price{99});
    EXPECT_EQ(book.best_ask(), Price{101});
    EXPECT_FALSE(book.crossed());
}

// ---------- matching ----------
TEST(BookMatch, FullFill) {
    Book book;
    book.add(limit(1, Side::Sell, 100, 10));
    auto evs = book.add(limit(2, Side::Buy, 100, 10));
    auto trades = only<Trade>(evs);
    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0], (Trade{OrderId{1}, OrderId{2}, Price{100}, Quantity{10}}));
    EXPECT_EQ(book.open_orders(), 0u);
    EXPECT_EQ(book.levels(Side::Sell), 0u);
}

TEST(BookMatch, PartialFillRestsRemainder) {
    Book book;
    book.add(limit(1, Side::Sell, 100, 4));
    auto evs = book.add(limit(2, Side::Buy, 100, 10));
    ASSERT_EQ(only<Trade>(evs).size(), 1u);
    EXPECT_EQ(book.open_orders(), 1u);
    EXPECT_EQ(book.best_bid(), Price{100});
    EXPECT_EQ(book.volume_at(Side::Buy, Price{100}), Quantity{6});
    EXPECT_FALSE(book.best_ask().has_value());
}

TEST(BookMatch, CrossesSeveralLevels) {
    Book book;
    book.add(limit(1, Side::Sell, 100, 3));
    book.add(limit(2, Side::Sell, 101, 3));
    book.add(limit(3, Side::Sell, 102, 3));
    auto trades = only<Trade>(book.add(limit(4, Side::Buy, 101, 10)));
    ASSERT_EQ(trades.size(), 2u);
    EXPECT_EQ(trades[0].price, Price{100});
    EXPECT_EQ(trades[1].price, Price{101});
    EXPECT_EQ(book.best_bid(), Price{101});           // remainder 4 rests at its limit
    EXPECT_EQ(book.volume_at(Side::Buy, Price{101}), Quantity{4});
    EXPECT_EQ(book.best_ask(), Price{102});
    EXPECT_FALSE(book.crossed());
}

TEST(BookMatch, TradesAtRestingPrice) {
    Book book;
    book.add(limit(1, Side::Sell, 100, 5));
    auto trades = only<Trade>(book.add(limit(2, Side::Buy, 105, 5)));
    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].price, Price{100});            // not 105
}

TEST(BookMatch, TimePriorityWithinLevel) {
    Book book;
    book.add(limit(1, Side::Sell, 100, 5));
    book.add(limit(2, Side::Sell, 100, 5));
    auto trades = only<Trade>(book.add(limit(3, Side::Buy, 100, 7)));
    ASSERT_EQ(trades.size(), 2u);
    EXPECT_EQ(trades[0].resting, OrderId{1});
    EXPECT_EQ(trades[0].qty, Quantity{5});
    EXPECT_EQ(trades[1].resting, OrderId{2});
    EXPECT_EQ(trades[1].qty, Quantity{2});
    EXPECT_EQ(book.volume_at(Side::Sell, Price{100}), Quantity{3});
}

TEST(BookMatch, MarketOrderEmptySide) {
    Book book;
    auto evs = book.add(market(1, Side::Buy, 10));
    ASSERT_EQ(evs.size(), 1u);
    EXPECT_EQ(std::get<Rejected>(evs[0]).reason, RejectReason::NoLiquidity);
    EXPECT_EQ(book.open_orders(), 0u);
}

TEST(BookMatch, MarketRemainderIsCancelledNotRested) {
    Book book;
    book.add(limit(1, Side::Sell, 100, 4));
    auto evs = book.add(market(2, Side::Buy, 10));
    ASSERT_EQ(only<Trade>(evs).size(), 1u);
    auto c = only<Cancelled>(evs);
    ASSERT_EQ(c.size(), 1u);
    EXPECT_EQ(c[0].remaining, Quantity{6});
    EXPECT_EQ(book.open_orders(), 0u);
}

// ---------- rejects ----------
TEST(BookReject, ZeroQuantity) {
    Book book;
    auto evs = book.add(limit(1, Side::Buy, 100, 0));
    ASSERT_EQ(evs.size(), 1u);
    EXPECT_EQ(std::get<Rejected>(evs[0]).reason, RejectReason::ZeroQuantity);
}

TEST(BookReject, DuplicateId) {
    Book book;
    book.add(limit(1, Side::Buy, 100, 1));
    auto evs = book.add(limit(1, Side::Buy, 99, 1));
    ASSERT_EQ(evs.size(), 1u);
    EXPECT_EQ(std::get<Rejected>(evs[0]).reason, RejectReason::DuplicateId);
    EXPECT_EQ(book.open_orders(), 1u);
}

// ---------- cancel ----------
TEST(BookCancel, CancelFromMiddle) {
    Book book;
    book.add(limit(1, Side::Sell, 100, 1));
    book.add(limit(2, Side::Sell, 100, 2));
    book.add(limit(3, Side::Sell, 100, 3));
    auto evs = book.cancel(OrderId{2});
    ASSERT_EQ(evs.size(), 1u);
    EXPECT_EQ(std::get<Cancelled>(evs[0]).remaining, Quantity{2});
    EXPECT_EQ(book.volume_at(Side::Sell, Price{100}), Quantity{4});
    auto trades = only<Trade>(book.add(limit(4, Side::Buy, 100, 4)));
    ASSERT_EQ(trades.size(), 2u);
    EXPECT_EQ(trades[0].resting, OrderId{1});
    EXPECT_EQ(trades[1].resting, OrderId{3});
}

TEST(BookCancel, UnknownId) {
    Book book;
    auto evs = book.cancel(OrderId{42});
    ASSERT_EQ(evs.size(), 1u);
    EXPECT_EQ(std::get<Rejected>(evs[0]).reason, RejectReason::UnknownId);
}

TEST(BookCancel, LastOrderErasesLevel) {
    Book book;
    book.add(limit(1, Side::Buy, 100, 1));
    book.cancel(OrderId{1});
    EXPECT_EQ(book.levels(Side::Buy), 0u);
    EXPECT_FALSE(book.best_bid().has_value());
}

TEST(BookCancel, DoubleCancel) {
    Book book;
    book.add(limit(1, Side::Buy, 100, 1));
    book.cancel(OrderId{1});
    auto evs = book.cancel(OrderId{1});
    EXPECT_EQ(std::get<Rejected>(evs[0]).reason, RejectReason::UnknownId);
}

// ---------- modify ----------
TEST(BookModify, SizeDownKeepsPriority) {
    Book book;
    book.add(limit(1, Side::Sell, 100, 10));
    book.add(limit(2, Side::Sell, 100, 10));
    auto evs = book.modify(OrderId{1}, Price{100}, Quantity{3});
    EXPECT_TRUE(std::holds_alternative<Modified>(evs.at(0)));
    auto trades = only<Trade>(book.add(limit(3, Side::Buy, 100, 3)));
    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].resting, OrderId{1});        // still first in line
}

TEST(BookModify, PriceChangeLosesPriority) {
    Book book;
    book.add(limit(1, Side::Sell, 100, 5));
    book.add(limit(2, Side::Sell, 100, 5));
    book.modify(OrderId{1}, Price{101}, Quantity{5});
    book.modify(OrderId{1}, Price{100}, Quantity{5});   // back at 100, now behind #2
    auto trades = only<Trade>(book.add(limit(3, Side::Buy, 100, 5)));
    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].resting, OrderId{2});
}

TEST(BookModify, SizeUpLosesPriority) {
    Book book;
    book.add(limit(1, Side::Sell, 100, 5));
    book.add(limit(2, Side::Sell, 100, 5));
    book.modify(OrderId{1}, Price{100}, Quantity{8});
    auto trades = only<Trade>(book.add(limit(3, Side::Buy, 100, 5)));
    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].resting, OrderId{2});
}

TEST(BookModify, PriceChangeCanTrade) {
    Book book;
    book.add(limit(1, Side::Sell, 101, 5));
    book.add(limit(2, Side::Buy, 99, 5));
    auto trades = only<Trade>(book.modify(OrderId{2}, Price{101}, Quantity{5}));
    ASSERT_EQ(trades.size(), 1u);
    EXPECT_EQ(trades[0].price, Price{101});
    EXPECT_EQ(book.open_orders(), 0u);
}

// ---------- whole-book properties ----------
TEST(BookProperty, RandomOpsMatchReference) {
    for (std::uint64_t seed : {1u, 2u, 3u, 4u, 5u}) {
        Book book;
        RefBook ref;
        auto ops = random_ops(seed, 5000);
        for (std::size_t i = 0; i < ops.size(); ++i) {
            auto got = apply(book, ops[i]);
            auto want = apply(ref, ops[i]);
            ASSERT_TRUE(got == want) << "seed " << seed << " op " << i;
            ASSERT_FALSE(book.crossed()) << "seed " << seed << " op " << i;
            ASSERT_EQ(book.best_bid(), ref.best_price(Side::Buy)) << "op " << i;
            ASSERT_EQ(book.best_ask(), ref.best_price(Side::Sell)) << "op " << i;
            ASSERT_EQ(book.open_orders(), ref.open_orders()) << "op " << i;
        }
    }
}

TEST(BookProperty, DeterministicReplay) {
    auto ops = random_ops(99, 20000);
    Book a, b;
    for (const auto& op : ops) {
        ASSERT_TRUE(apply(a, op) == apply(b, op));
    }
}

// ---------- threads (run under the TSan build) ----------
TEST(LockedBook, ConcurrentAddsNoRace) {
    LockedBook book;
    constexpr int kThreads = 4, kPer = 2000;
    std::vector<std::thread> ts;
    for (int t = 0; t < kThreads; ++t) {
        ts.emplace_back([&book, t] {
            for (int i = 0; i < kPer; ++i) {
                std::uint64_t id = static_cast<std::uint64_t>(t) * kPer + i + 1;
                // Buys below 100, sells above: never cross, so every order rests.
                if (t % 2 == 0) book.add(limit(id, Side::Buy, 90 + i % 10, 1));
                else            book.add(limit(id, Side::Sell, 101 + i % 10, 1));
            }
        });
    }
    for (auto& th : ts) th.join();
    EXPECT_EQ(book.open_orders(), static_cast<std::size_t>(kThreads * kPer));
    EXPECT_FALSE(book.crossed());
}
