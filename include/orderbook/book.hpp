#pragma once
#include <functional>
#include <map>
#include <unordered_map>
#include <vector>
#include "orderbook/events.hpp"
#include "orderbook/price_level.hpp"

namespace ob {

class Book {
public:
    std::vector<Event> add(Order o);
    std::vector<Event> cancel(OrderId id);
    std::vector<Event> modify(OrderId id, Price new_price, Quantity new_qty);

    // Test helpers
    bool crossed() const;            // best bid >= best ask
    std::size_t open_orders() const;

private:
    using Bids = std::map<Price, PriceLevel, std::greater<>>;
    using Asks = std::map<Price, PriceLevel>;

    struct Locator {
        Side side;
        Price price;                 // re-find the level by key, see note below
        PriceLevel::Handle handle;
    };

    Bids bids_;
    Asks asks_;
    std::unordered_map<OrderId, Locator> index_;
};

}  // namespace ob
