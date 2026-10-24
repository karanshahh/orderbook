include(GoogleTest)

add_executable(smoke_test smoke_test.cpp)
target_link_libraries(smoke_test PRIVATE orderbook orderbook_warnings GTest::gtest_main)
gtest_discover_tests(smoke_test)

add_executable(orderbook_test orderbook_test.cpp)
target_link_libraries(orderbook_test PRIVATE orderbook orderbook_warnings GTest::gtest_main)
gtest_discover_tests(orderbook_test)
