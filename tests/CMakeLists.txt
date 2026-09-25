# GoogleTest: use an installed copy if present, otherwise fetch it.
# @spec DLV-BUILD-004
find_package(GTest CONFIG QUIET)
if(NOT GTest_FOUND)
  include(FetchContent)
  FetchContent_Declare(googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.15.2.tar.gz
    URL_HASH SHA256=7b42b4d6ed48810c5362c265a17faebe90dc2373c885e5216439d37927f02926
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)
endif()
include(GoogleTest)

# Differential-test size: 10^6 requests per profile normally, 10^5 under
# sanitizers (DLV-TEST-002). Override with -DMATCHER_DIFF_REQUESTS=N.
if(NOT DEFINED MATCHER_DIFF_REQUESTS)
  if(MATCHER_SANITIZE OR CMAKE_BUILD_TYPE STREQUAL "Debug")
    set(MATCHER_DIFF_REQUESTS 100000)
  else()
    set(MATCHER_DIFF_REQUESTS 1000000)
  endif()
endif()

# Shared test support: reference engine, request generator, fakes.
add_library(matcher_test_support
  support/reference_engine.cpp
  support/request_generator.cpp)
target_include_directories(matcher_test_support PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(matcher_test_support PUBLIC matcher_core PRIVATE matcher_warnings)

add_executable(matcher_tests
  unit/price_test.cpp
  unit/node_pool_test.cpp
  unit/order_index_test.cpp
  unit/level_store_test.cpp
  unit/order_book_test.cpp
  unit/matching_engine_test.cpp
  unit/line_reader_test.cpp
  unit/request_parser_test.cpp
  unit/buffered_writer_test.cpp
  unit/event_writer_test.cpp
  unit/error_reporter_test.cpp
  unit/app_test.cpp
  scenario/brief_example_test.cpp
  scenario/priority_test.cpp
  scenario/rejection_test.cpp
  scenario/edge_values_test.cpp
  property/differential_test.cpp
  property/book_invariants_test.cpp)
target_compile_definitions(matcher_tests PRIVATE MATCHER_DIFF_REQUESTS=${MATCHER_DIFF_REQUESTS})
target_link_libraries(matcher_tests PRIVATE matcher_test_support GTest::gtest_main matcher_warnings)
gtest_discover_tests(matcher_tests DISCOVERY_TIMEOUT 60 PROPERTIES TIMEOUT 1800)

# Replaces global operator new, so it must be its own binary.
add_executable(no_alloc_test alloc/no_alloc_test.cpp)
target_link_libraries(no_alloc_test PRIVATE matcher_test_support GTest::gtest_main matcher_warnings)
gtest_discover_tests(no_alloc_test DISCOVERY_TIMEOUT 60)

# Golden end-to-end tests: run the real binary on every data/golden/*.in.
file(GLOB GOLDEN_INPUTS CONFIGURE_DEPENDS ${PROJECT_SOURCE_DIR}/data/golden/*.in)
foreach(input ${GOLDEN_INPUTS})
  get_filename_component(name ${input} NAME_WE)
  add_test(NAME golden.${name}
    COMMAND ${CMAKE_COMMAND}
      -DMATCHER=$<TARGET_FILE:matcher>
      -DCASE=${PROJECT_SOURCE_DIR}/data/golden/${name}
      -P ${CMAKE_CURRENT_SOURCE_DIR}/e2e/run_golden.cmake)
endforeach()
