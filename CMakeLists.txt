cmake_minimum_required(VERSION 3.20)
project(relay CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  # A proxy benchmark at -O0 measures nothing: parsing and balancing are both
  # tight loops, and an unoptimized build is the easiest way to publish a
  # throughput number that is really just the optimizer being switched off.
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

set(CMAKE_CXX_FLAGS_RELWITHDEBINFO "-O2 -g -DNDEBUG")
add_compile_options(-Wall -Wextra -Wpedantic -Wno-unused-parameter)

option(RELAY_ASAN "Build with address and undefined-behaviour sanitizers" OFF)
if(RELAY_ASAN)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

# A separate build rather than another flag on the ASAN one, because clang
# refuses to combine them -- they instrument the same loads and stores
# incompatibly. Keeping them apart also keeps what each one proves apart: ASAN
# covers the parser's bounds and the pool's lifetimes, TSAN covers the
# snapshot-publication story between the control-plane thread and the workers,
# and a single mixed run that failed would not say which.
option(RELAY_TSAN "Build with the thread sanitizer" OFF)
if(RELAY_TSAN)
  if(RELAY_ASAN)
    message(FATAL_ERROR "RELAY_ASAN and RELAY_TSAN cannot be combined; configure two build dirs")
  endif()
  add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
  add_link_options(-fsanitize=thread)
endif()

include_directories(${CMAKE_SOURCE_DIR}/data-plane/include)

add_library(relay_core STATIC
  data-plane/src/core/http.cpp
  data-plane/src/core/metrics.cpp
  data-plane/src/upstream/balancer.cpp
  data-plane/src/upstream/pool.cpp
  data-plane/src/policy/policy.cpp
  data-plane/src/proxy/router.cpp
  data-plane/src/proxy/socket.cpp
  data-plane/src/proxy/server.cpp
)
target_include_directories(relay_core PUBLIC ${CMAKE_SOURCE_DIR}/data-plane/include)

find_package(Threads REQUIRED)
target_link_libraries(relay_core PUBLIC Threads::Threads)

enable_testing()

# One binary per test file. A single binary would be faster to link and makes
# a crash in one area take down the whole suite with no indication of which
# area, which during the connection-pool work was most of a day.
set(RELAY_TESTS
  http_test
  balancer_test
  ring_test
  breaker_test
  retry_test
  limiter_test
  shedder_test
  metrics_test
  pool_test
  router_test
  smuggling_test
)

foreach(test_name ${RELAY_TESTS})
  add_executable(${test_name} data-plane/tests/${test_name}.cpp)
  target_link_libraries(${test_name} PRIVATE relay_core)
  target_include_directories(${test_name} PRIVATE ${CMAKE_SOURCE_DIR}/data-plane/tests)
  add_test(NAME ${test_name} COMMAND ${test_name})
endforeach()

add_executable(relay-proxy data-plane/tools/relay_proxy_main.cpp)
target_link_libraries(relay-proxy PRIVATE relay_core)

add_executable(relay-bench data-plane/tools/bench_main.cpp)
target_link_libraries(relay-bench PRIVATE relay_core)

# A deliberately misbehaving origin server for the stress tests: configurable
# latency, error rate, and the ability to go away entirely. Without one, every
# failure-handling test would need a real upstream to break on command.
add_executable(relay-origin data-plane/tools/origin_main.cpp)
target_link_libraries(relay-origin PRIVATE relay_core)
