include(GoogleTest)

add_executable(smoke_test smoke_test.cpp)
target_link_libraries(smoke_test PRIVATE orderbook orderbook_warnings GTest::gtest_main)
gtest_discover_tests(smoke_test)

add_executable(orderbook_test orderbook_test.cpp)
target_link_libraries(orderbook_test PRIVATE orderbook orderbook_warnings GTest::gtest_main Threads::Threads)
gtest_discover_tests(orderbook_test)

add_executable(itch_test itch_test.cpp)
target_link_libraries(itch_test PRIVATE itch orderbook_warnings GTest::gtest_main)
gtest_discover_tests(itch_test)

# Replaces global operator new to count allocations, so it is its own program.
add_executable(alloc_test alloc_test.cpp)
target_link_libraries(alloc_test PRIVATE orderbook orderbook_warnings GTest::gtest_main)
gtest_discover_tests(alloc_test)
