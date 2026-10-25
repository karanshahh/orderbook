cmake_minimum_required(VERSION 3.21)
project(orderbook LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Sanitizers: -DSANITIZE=address,undefined  or  -DSANITIZE=thread
set(SANITIZE "" CACHE STRING "Sanitizers to enable")
if(SANITIZE)
  # Global on purpose: GoogleTest must be instrumented too (matters most for TSan)
  add_compile_options(-fsanitize=${SANITIZE} -fno-omit-frame-pointer)
  add_link_options(-fsanitize=${SANITIZE})
endif()

find_package(Threads REQUIRED)

# Warnings: opt-in per target, so third-party code is not affected
add_library(orderbook_warnings INTERFACE)
target_compile_options(orderbook_warnings INTERFACE -Wall -Wextra -Wpedantic)

# Order book engines: v1 (std containers) and v2 (pool, intrusive lists, flat arrays)
add_library(orderbook STATIC src/book.cpp src/price_level.cpp src/v2/book.cpp)
target_include_directories(orderbook PUBLIC include)
target_compile_features(orderbook PUBLIC cxx_std_23)
target_link_libraries(orderbook PRIVATE orderbook_warnings)

# Nasdaq ITCH 5.0 decoder and replay builder
add_library(itch STATIC src/itch/itch.cpp src/itch/replay.cpp)
target_link_libraries(itch PUBLIC orderbook PRIVATE orderbook_warnings)

# Programs (only meaningful in a Release build without sanitizers)
add_executable(orderbook_bench bench/bench.cpp)
target_link_libraries(orderbook_bench PRIVATE orderbook orderbook_warnings Threads::Threads)

add_executable(itch_replay tools/itch_replay.cpp)
target_link_libraries(itch_replay PRIVATE itch orderbook_warnings)

if(PROJECT_IS_TOP_LEVEL)
include(FetchContent)
FetchContent_Declare(googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(googletest)

enable_testing()
add_subdirectory(tests)
endif()
