#include "orderbook/book.hpp"

namespace ob {

std::vector<Event> Book::add(Order o) {
    // TODO
    return {};
}

std::vector<Event> Book::cancel(OrderId id) {
    // TODO
    return {};
}

std::vector<Event> Book::modify(OrderId id, Price new_price, Quantity new_qty) {
    // TODO
    return {};
}

bool Book::crossed() const {
    // TODO
    return false;
}

std::size_t Book::open_orders() const {
    // TODO
    return 0;
}

}  // namespace ob
