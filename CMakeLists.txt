cmake_minimum_required(VERSION 3.20)
project(hws VERSION 2.0.0 DESCRIPTION "High-performance HTTP/1.1 server for Linux" LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(HWS_BUILD_TESTS "Build unit tests (fetches GoogleTest)" ON)
option(HWS_BUILD_TOOLS "Build benchmark/load tools" ON)
option(HWS_BUILD_FUZZERS "Build libFuzzer targets (requires clang)" OFF)
option(HWS_WERROR "Treat compiler warnings as errors" OFF)
option(HWS_LTO "Enable link-time optimization for release builds" ON)
option(HWS_NATIVE "Tune for the build machine (-march=native)" OFF)
option(HWS_STATIC_RUNTIME "Link libstdc++/libgcc statically (binary needs only glibc)" OFF)
set(HWS_SANITIZE "" CACHE STRING "Comma-separated sanitizers, e.g. address,undefined or thread")

find_package(Threads REQUIRED)

add_library(hws_options INTERFACE)
target_compile_options(hws_options INTERFACE
  -Wall -Wextra -Wpedantic -Wshadow -Wnon-virtual-dtor -Wold-style-cast -Wcast-align
  -Woverloaded-virtual -Wformat=2 -Wimplicit-fallthrough -Wmissing-declarations)
if(HWS_WERROR)
  target_compile_options(hws_options INTERFACE -Werror)
endif()
if(HWS_NATIVE)
  target_compile_options(hws_options INTERFACE -march=native)
endif()

if(HWS_SANITIZE)
  add_compile_options(-fsanitize=${HWS_SANITIZE} -fno-omit-frame-pointer -fno-sanitize-recover=all)
  add_link_options(-fsanitize=${HWS_SANITIZE})
endif()

if(HWS_BUILD_FUZZERS)
  add_compile_options(-fsanitize=fuzzer-no-link)  # coverage instrumentation for everything fuzzed
endif()

if(HWS_LTO AND CMAKE_BUILD_TYPE MATCHES "Release|RelWithDebInfo" AND NOT HWS_SANITIZE AND NOT HWS_BUILD_FUZZERS)
  include(CheckIPOSupported)
  check_ipo_supported(RESULT hws_ipo_ok OUTPUT hws_ipo_msg)
  if(hws_ipo_ok)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
  endif()
endif()

add_library(hws_core STATIC
  src/util/buffer.cpp
  src/util/log.cpp
  src/http/chunked.cpp
  src/http/date.cpp
  src/http/mime.cpp
  src/http/parser.cpp
  src/http/range.cpp
  src/http/response.cpp
  src/http/router.cpp
  src/http/status.cpp
  src/http/uri.cpp
  src/net/event_loop.cpp
  src/net/socket.cpp
  src/server/access_log.cpp
  src/server/config.cpp
  src/server/connection.cpp
  src/server/file_cache.cpp
  src/server/metrics.cpp
  src/server/server.cpp
  src/server/static_files.cpp
  src/server/worker.cpp)
target_include_directories(hws_core PUBLIC src)
target_compile_definitions(hws_core PUBLIC HWS_VERSION="${PROJECT_VERSION}")
target_link_libraries(hws_core PUBLIC Threads::Threads PRIVATE hws_options)

add_executable(hws src/main.cpp)
target_link_libraries(hws PRIVATE hws_core hws_options)
if(HWS_STATIC_RUNTIME)
  target_link_options(hws PRIVATE -static-libstdc++ -static-libgcc)
endif()
install(TARGETS hws RUNTIME DESTINATION bin)

if(HWS_BUILD_TOOLS)
  add_executable(hws-connhold tools/connhold.cpp)
  target_link_libraries(hws-connhold PRIVATE Threads::Threads hws_options)
endif()

if(HWS_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

if(HWS_BUILD_FUZZERS)
  add_subdirectory(fuzz)
endif()
