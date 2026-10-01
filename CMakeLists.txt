cmake_minimum_required(VERSION 3.20)
project(SchemaProvider VERSION 0.0.0 LANGUAGES CXX)

# Proof-of-concept build. Nothing here is an installed or stable interface yet; see docs/POC_PLAN.md.

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

option(SP_BUILD_TESTS "Build the transport property tests and the loopback server" ON)
set(SP_SANITIZE "" CACHE STRING "Sanitizer set: address, thread (undefined is added to address)")

if(SP_SANITIZE STREQUAL "address")
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined)
  add_link_options(-fsanitize=address,undefined)
elseif(SP_SANITIZE STREQUAL "thread")
  add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
  add_link_options(-fsanitize=thread)
elseif(NOT SP_SANITIZE STREQUAL "")
  message(FATAL_ERROR "SP_SANITIZE must be empty, 'address' or 'thread'")
endif()

find_package(Threads REQUIRED)
find_package(CURL 7.88 REQUIRED)
find_path(SP_ASIO_INCLUDE_DIR asio.hpp HINTS "${ASIO_ROOT}/include" DOC "standalone Asio headers")
if(NOT SP_ASIO_INCLUDE_DIR)
  message(FATAL_ERROR "standalone Asio not found: install libasio-dev or pass -DASIO_ROOT=<dir containing include/asio.hpp>")
endif()

add_library(sp_transport STATIC src/transport/http_transport.cpp)
target_include_directories(sp_transport
  PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/src
  PRIVATE ${SP_ASIO_INCLUDE_DIR})
target_compile_definitions(sp_transport PRIVATE ASIO_STANDALONE ASIO_NO_DEPRECATED)
target_compile_options(sp_transport PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(sp_transport PUBLIC Threads::Threads PRIVATE CURL::libcurl)

if(SP_BUILD_TESTS)
  enable_testing()

  add_executable(sp_loopback_server tests/support/loopback_server.cpp)
  target_include_directories(sp_loopback_server PRIVATE ${SP_ASIO_INCLUDE_DIR})
  target_compile_definitions(sp_loopback_server PRIVATE ASIO_STANDALONE ASIO_NO_DEPRECATED)
  target_compile_options(sp_loopback_server PRIVATE -Wall -Wextra -Wpedantic)
  target_link_libraries(sp_loopback_server PRIVATE Threads::Threads)

  add_executable(sp_transport_tests tests/transport_properties_test.cpp)
  target_compile_options(sp_transport_tests PRIVATE -Wall -Wextra -Wpedantic)
  target_link_libraries(sp_transport_tests PRIVATE sp_transport)

  # ThreadSanitizer's shadow memory needs a fixed address-space layout; kernels with high mmap
  # entropy (this WSL2 kernel) abort with "unexpected memory mapping" unless ASLR is off.
  set(sp_test_runner "")
  if(SP_SANITIZE STREQUAL "thread")
    find_program(SP_SETARCH setarch)
    if(SP_SETARCH)
      set(sp_test_runner ${SP_SETARCH} ${CMAKE_SYSTEM_PROCESSOR} -R)
    endif()
  endif()

  add_test(NAME transport_properties
           COMMAND ${sp_test_runner} $<TARGET_FILE:sp_transport_tests> $<TARGET_FILE:sp_loopback_server>
                   ${CMAKE_CURRENT_SOURCE_DIR}/tests/support/h2c_flood_server.mjs)
  set_tests_properties(transport_properties PROPERTIES TIMEOUT 300)
endif()
