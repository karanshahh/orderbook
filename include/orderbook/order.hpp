#pragma once
#include "orderbook/types.hpp"

namespace ob {

struct Order {
    OrderId   id;
    Side      side;
    OrderType type;
    Price     price;      // ignored for Market
    Quantity  remaining;

    bool operator==(const Order&) const = default;
};

}  // namespace ob
