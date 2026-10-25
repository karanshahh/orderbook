#pragma once
#include <cstddef>
#include <list>
#include "orderbook/order.hpp"

namespace ob {

// All resting orders at one price, oldest first (time priority).
// std::list: O(1) erase from the middle, and erasing one element never
// invalidates iterators (Handles) to the others.
class PriceLevel {
public:
    using Orders = std::list<Order>;
    using Handle = Orders::iterator;

    Handle append(const Order& o);
    void   erase(Handle h);
    void   reduce(Handle h, Quantity by);   // partial fill or size-down modify

    bool          empty() const;
    std::size_t   size() const;
    Quantity      total() const;
    Order&        front();
    Handle        front_handle();
    const Orders& orders() const;

private:
    Orders   orders_;
    Quantity total_{};
};

}  // namespace ob
