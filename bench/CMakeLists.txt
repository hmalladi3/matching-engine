# Benchmark harness. Uses the test-support request generator for workloads.
if(NOT TARGET matcher_test_support)
  message(STATUS "matcher_bench needs MATCHER_BUILD_TESTS=ON (for the request generator); skipping")
  return()
endif()

# Record the exact flags in the binary so results can state how they were built.
string(TOUPPER "${CMAKE_BUILD_TYPE}" build_type_upper)
string(STRIP "${CMAKE_CXX_FLAGS} ${CMAKE_CXX_FLAGS_${build_type_upper}} [${CMAKE_BUILD_TYPE}]" MATCHER_BENCH_FLAGS)
configure_file(bench_config.h.in ${CMAKE_CURRENT_BINARY_DIR}/bench_config.h @ONLY)

add_executable(matcher_bench bench_main.cpp)
target_include_directories(matcher_bench PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
target_link_libraries(matcher_bench PRIVATE matcher_test_support matcher_warnings)
