include(FetchContent)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
FetchContent_Declare(googletest
  URL https://github.com/google/googletest/releases/download/v1.15.2/googletest-1.15.2.tar.gz
  URL_HASH SHA256=7b42b4d6ed48810c5362c265a17faebe90dc2373c885e5216439d37927f02926
  DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
FetchContent_MakeAvailable(googletest)

add_executable(hws_unit_tests
  unit/buffer_test.cpp
  unit/chunked_test.cpp
  unit/config_test.cpp
  unit/date_test.cpp
  unit/parser_test.cpp
  unit/range_test.cpp
  unit/router_test.cpp
  unit/timer_wheel_test.cpp
  unit/uri_test.cpp)
target_link_libraries(hws_unit_tests PRIVATE hws_core hws_options GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(hws_unit_tests DISCOVERY_TIMEOUT 30)

find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
  add_test(NAME integration
           COMMAND ${Python3_EXECUTABLE} -I ${CMAKE_CURRENT_SOURCE_DIR}/integration/test_server.py
                   --binary $<TARGET_FILE:hws>)
  set_tests_properties(integration PROPERTIES TIMEOUT 300)
endif()
