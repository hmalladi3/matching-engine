# C++20, Linux (GCC 10+ / Clang 12+), no third-party dependencies outside tests.
cmake_minimum_required(VERSION 3.20)
project(order_matcher VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(MATCHER_BUILD_TESTS "Build unit, scenario, property and e2e tests" ON)
option(MATCHER_BUILD_BENCH "Build the benchmark harness" ON)
option(MATCHER_BUILD_FUZZ  "Build libFuzzer targets (Clang only)" OFF)
option(MATCHER_SANITIZE    "Build everything with ASan + UBSan" OFF)
option(MATCHER_COVERAGE    "Instrument for source-based coverage (Clang)" OFF)
option(MATCHER_WERROR      "Treat warnings as errors" ON)

# Strict warnings for our own targets only (never for third-party code).
add_library(matcher_warnings INTERFACE)
target_compile_options(matcher_warnings INTERFACE
  -Wall -Wextra -Wpedantic -Wconversion -Wsign-conversion -Wshadow
  $<$<BOOL:${MATCHER_WERROR}>:-Werror>)

if(MATCHER_SANITIZE)
  # -O1 is the usual optimization level for sanitizer runs: full checking,
  # several times faster than -O0 (assertions stay enabled in Debug).
  add_compile_options(-fsanitize=address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer -O1)
  add_link_options(-fsanitize=address,undefined)
endif()

if(MATCHER_COVERAGE)
  add_compile_options(-fprofile-instr-generate -fcoverage-mapping)
  add_link_options(-fprofile-instr-generate)
endif()

# ---- core library: everything except main() --------------------------------
add_library(matcher_core
  src/price.cpp
  src/node_pool.cpp
  src/order_index.cpp
  src/level_store.cpp
  src/order_book.cpp
  src/line_reader.cpp
  src/request_parser.cpp
  src/buffered_writer.cpp
  src/event_writer.cpp
  src/error_reporter.cpp
  src/app.cpp)
target_include_directories(matcher_core PUBLIC include)
target_link_libraries(matcher_core PRIVATE matcher_warnings)

# ---- executable ---------------------------------------------------------------
add_executable(matcher app/main.cpp)
target_link_libraries(matcher PRIVATE matcher_core matcher_warnings)

if(MATCHER_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
  # Seeded dataset generator (shares the tests' request generator).
  add_executable(gen_orders tools/gen_orders.cpp)
  target_link_libraries(gen_orders PRIVATE matcher_test_support matcher_warnings)
endif()

# Stress-suite helpers (scripts/stress.sh).
add_executable(slow_reader tools/slow_reader.cpp)
target_link_libraries(slow_reader PRIVATE matcher_warnings)
add_executable(measure tools/measure.cpp)
target_link_libraries(measure PRIVATE matcher_warnings)

if(MATCHER_BUILD_BENCH AND EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/bench/CMakeLists.txt)
  add_subdirectory(bench)
endif()

if(MATCHER_BUILD_FUZZ)
  add_subdirectory(fuzz)
endif()
