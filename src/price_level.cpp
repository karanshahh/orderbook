#include "orderbook/price_level.hpp"

namespace ob {

PriceLevel::Handle PriceLevel::append(const Order& o) {
    // TODO: push_back, add o.remaining to total_, return iterator to new element
    return orders_.end();
}

void PriceLevel::erase(Handle h) {
    // TODO: subtract h->remaining from total_, erase h
}

bool PriceLevel::empty() const {
    // TODO
    return true;
}

Quantity PriceLevel::total() const {
    // TODO
    return total_;
}

Order& PriceLevel::front() {
    // TODO
    return orders_.front();
}

}  // namespace ob
