# libFuzzer targets (Clang only): build with -DMATCHER_BUILD_FUZZ=ON.
foreach(target fuzz_pipeline fuzz_price)
  add_executable(${target} ${target}.cpp)
  target_link_libraries(${target} PRIVATE matcher_core matcher_warnings)
  target_compile_options(${target} PRIVATE -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=all)
  target_link_options(${target} PRIVATE -fsanitize=fuzzer,address,undefined)
endforeach()
