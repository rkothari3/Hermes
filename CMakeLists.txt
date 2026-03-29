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
    src/latency_profiler.cpp
)
target_include_directories(hermes_lib PUBLIC include)
target_link_libraries(hermes_lib pthread)

# Main binary
add_executable(hermes src/main.cpp)
target_link_libraries(hermes hermes_lib)

# Google Test via FetchContent — DISCONNECTED avoids re-fetching after first download
include(FetchContent)
set(FETCHCONTENT_UPDATES_DISCONNECTED ON)
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG        v1.14.0
)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

# Unit tests (fast — no file I/O, runs in <1s)
enable_testing()
add_executable(hermes_tests
    tests/test_parser.cpp
    tests/test_order_book.cpp
    tests/test_signals.cpp
    tests/test_latency_profiler.cpp
)
target_link_libraries(hermes_tests hermes_lib GTest::gtest_main)
include(GoogleTest)
gtest_discover_tests(hermes_tests)

# Integration test (slow — reads full ITCH file, run separately)
set(ITCH_FILE "" CACHE STRING "Path to ITCH file for integration test")
if(ITCH_FILE)
    add_executable(hermes_integration_tests
        tests/test_integration.cpp
    )
    target_link_libraries(hermes_integration_tests hermes_lib GTest::gtest_main)
    target_compile_definitions(hermes_integration_tests PRIVATE ITCH_FILE_PATH="${ITCH_FILE}")
    gtest_discover_tests(hermes_integration_tests)
endif()
