// book.cpp
#include "orderbook/book.hpp"
namespace ob {
std::vector<Event> Book::add(Order) { return {}; }
std::vector<Event> Book::cancel(OrderId) { return {}; }
std::vector<Event> Book::modify(OrderId, Price, Quantity) { return {}; }
bool Book::crossed() const { return false; }
std::size_t Book::open_orders() const { return 0; }
}
