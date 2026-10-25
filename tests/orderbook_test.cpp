#include <gtest/gtest.h>
#include <variant>
#include "orderbook/book.hpp"

using namespace ob;

TEST(BookAdd, RestingLimitBuy) {
    Book book;
    Order order{OrderId{1},Side::Buy,OrderType::Limit,Price{100},Quantity{10}};
    auto events = book.add(order);

    // assertions
    ASSERT_EQ(events.size(),1u);
    EXPECT_TRUE(std::holds_alternative<Accepted>(events[0]));
    EXPECT_EQ(book.open_orders(),1u);
    EXPECT_FALSE(book.crossed());
}
