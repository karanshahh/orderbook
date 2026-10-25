#pragma once
#include <cstddef>
#include <functional>
#include <map>
#include <optional>
#include <unordered_map>
#include <vector>
#include "orderbook/events.hpp"
#include "orderbook/price_level.hpp"

namespace ob {

// Single-instrument limit order book, price-time priority. Single-threaded.
//
// Event rules:
//   add:    invalid -> {Rejected}.
//           Market order against an empty opposite side -> {Rejected NoLiquidity}.
//           Otherwise {Accepted, Trade...}; a Limit remainder rests,
//           a Market remainder is dropped with a trailing {Cancelled}.
//   cancel: {Cancelled} or {Rejected UnknownId}.
//   modify: same price and qty <= current -> reduce in place, keeps priority,
//           {Modified}. Anything else -> cancel + re-add (loses priority),
//           returns the events of the re-add.
class Book {
public:
    std::vector<Event> add(Order o);
    std::vector<Event> cancel(OrderId id);
    std::vector<Event> modify(OrderId id, Price new_price, Quantity new_qty);

    // Queries
    std::optional<Price> best_bid() const;
    std::optional<Price> best_ask() const;
    Quantity    volume_at(Side side, Price price) const;
    bool        crossed() const;        // best bid >= best ask; must never be true at rest
    std::size_t open_orders() const;
    std::size_t levels(Side side) const;

private:
    using Bids = std::map<Price, PriceLevel, std::greater<>>;   // best (highest) first
    using Asks = std::map<Price, PriceLevel>;                   // best (lowest) first

    // Where a resting order lives. We store the price, not a map iterator:
    // the order's handle stays valid until that order is erased, and the
    // level is re-found by key, so no stored iterator can dangle when a level
    // is erased.
    struct Locator {
        Side side;
        Price price;
        PriceLevel::Handle handle;
    };

    std::optional<RejectReason> validate(const Order& o) const;
    static bool can_match(const Order& incoming, Price resting_price);

    template <typename Levels>
    void match(Order& incoming, Levels& opposite, std::vector<Event>& out);

    template <typename Levels>
    void remove(Levels& levels, const Locator& loc);

    void rest(const Order& o);

    Bids bids_;
    Asks asks_;
    std::unordered_map<OrderId, Locator> index_;
};

}  // namespace ob
