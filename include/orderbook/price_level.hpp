#pragma once
#include <list>
#include "orderbook/order.hpp"

namespace ob {

class PriceLevel {
public:
    using Orders = std::list<Order>;
    using Handle = Orders::iterator;

    Handle append(const Order& o)    // TODO: push_back, update total, return handle
    void   erase(Handle h);          // TODO: update total, erase
    bool   empty() const;
    Quantity total() const;
    Order& front();                  // oldest order = highest time priority

private:
    Orders   orders_;
    Quantity total_{};
};

}  // namespace ob
