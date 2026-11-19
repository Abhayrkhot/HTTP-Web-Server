# libFuzzer targets. Configure with clang:
#   cmake -B build-fuzz -DCMAKE_CXX_COMPILER=clang++ -DHWS_BUILD_FUZZERS=ON \
#         -DHWS_SANITIZE=address,undefined -DHWS_BUILD_TESTS=OFF -DCMAKE_BUILD_TYPE=RelWithDebInfo
#   ./build-fuzz/fuzz/fuzz_parser -max_total_time=60 fuzz/corpus/parser
if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  message(FATAL_ERROR "HWS_BUILD_FUZZERS requires clang (libFuzzer)")
endif()

foreach(name parser chunked uri misc)
  add_executable(fuzz_${name} fuzz_${name}.cpp)
  target_link_libraries(fuzz_${name} PRIVATE hws_core)
  target_link_options(fuzz_${name} PRIVATE -fsanitize=fuzzer)
endforeach()
