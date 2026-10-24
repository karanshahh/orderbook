cmake_minimum_required(VERSION 3.20)
project(orderbook LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
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

# Warnings: opt-in per target, so third-party code is not affected
add_library(orderbook_warnings INTERFACE)
target_compile_options(orderbook_warnings INTERFACE -Wall -Wextra -Wpedantic)

# Header-only for now; becomes a real library once src/ has files
add_library(orderbook INTERFACE)
target_include_directories(orderbook INTERFACE include)
target_compile_features(orderbook INTERFACE cxx_std_20)

include(FetchContent)
FetchContent_Declare(googletest
  URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip)
FetchContent_MakeAvailable(googletest)

enable_testing()
add_subdirectory(tests)
