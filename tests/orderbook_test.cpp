#include <gtest/gtest.h>
#include <variant>
#include "orderbook/book.hpp"

using namespace ob;

TEST(BookAdd, RestingLimitBuy) {
    // TODO: make Book
    // TODO: add Order{OrderId{1}, Side::Buy, OrderType::Limit, Price{100}, Quantity{10}}
    // TODO: ASSERT_EQ events.size() == 1
    // TODO: EXPECT holds_alternative<Accepted>(events[0])
    // TODO: EXPECT open_orders() == 1 and !crossed()
}
