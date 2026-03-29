cmake_minimum_required(VERSION 3.16)
project(hermes CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
add_compile_options(-Wall -Wextra -O2)

# Parser as a library shared between hermes binary and tests
add_library(hermes_lib
    src/itch_parser.cpp
    src/order_book.cpp
    src/signal_engine.cpp
)
target_include_directories(hermes_lib PUBLIC include)

# Main binary
add_executable(hermes src/main.cpp)
target_link_libraries(hermes hermes_lib)

# Google Test via FetchContent (downloads automatically, no manual install)
include(FetchContent)
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.14.0
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

# Test binary
enable_testing()
add_executable(hermes_tests
    tests/test_parser.cpp
    tests/test_order_book.cpp
)
target_link_libraries(hermes_tests hermes_lib GTest::gtest_main)
include(GoogleTest)
gtest_discover_tests(hermes_tests)
