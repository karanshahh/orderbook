#include "orderbook/price_level.hpp"
#include <iterator>

namespace ob {

PriceLevel::Handle PriceLevel::append(const Order& o) {
    orders_.push_back(o);
    total_.value += o.remaining.value;
    return std::prev(orders_.end());
}

void PriceLevel::erase(Handle h) {
    total_.value -= h->remaining.value;   // read before erase: h dies on erase
    orders_.erase(h);
}

void PriceLevel::reduce(Handle h, Quantity by) {
    h->remaining.value -= by.value;
    total_.value -= by.value;
}

bool PriceLevel::empty() const { return orders_.empty(); }
std::size_t PriceLevel::size() const { return orders_.size(); }
Quantity PriceLevel::total() const { return total_; }
Order& PriceLevel::front() { return orders_.front(); }
PriceLevel::Handle PriceLevel::front_handle() { return orders_.begin(); }
const PriceLevel::Orders& PriceLevel::orders() const { return orders_; }

}  // namespace ob
