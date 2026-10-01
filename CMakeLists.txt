cmake_minimum_required(VERSION 3.20)
project(SchemaProvider VERSION 0.0.0 LANGUAGES C CXX)

# Proof-of-concept build. Nothing here is an installed or stable interface yet; see docs/POC_PLAN.md.

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

option(SP_BUILD_TESTS "Build the PoC property tests, fixture runner and loopback peers" ON)
option(SP_BUILD_NEOGRAPH_SPIKE "Build the optional read-only NeoGraph integration experiments" OFF)
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
find_package(OpenSSL REQUIRED COMPONENTS Crypto)
find_path(SP_ASIO_INCLUDE_DIR asio.hpp HINTS "${ASIO_ROOT}/include" DOC "standalone Asio headers")
if(NOT SP_ASIO_INCLUDE_DIR)
  message(FATAL_ERROR "standalone Asio not found: install libasio-dev or pass -DASIO_ROOT=<dir containing include/asio.hpp>")
endif()

# yyjson stays private. Use an installed library or an explicitly supplied source tree;
# configuration never downloads dependencies or replaces a system package.
set(YYJSON_ROOT "" CACHE PATH "yyjson installation or source directory")
find_path(SP_YYJSON_INCLUDE_DIR yyjson.h HINTS "${YYJSON_ROOT}" "${YYJSON_ROOT}/src" "${YYJSON_ROOT}/include")
if(NOT SP_YYJSON_INCLUDE_DIR)
  message(FATAL_ERROR "yyjson not found: install yyjson or pass -DYYJSON_ROOT=<source or installation directory>")
endif()
find_file(SP_YYJSON_SOURCE yyjson.c PATHS "${SP_YYJSON_INCLUDE_DIR}" NO_DEFAULT_PATH)
if(SP_YYJSON_SOURCE)
  add_library(sp_yyjson STATIC "${SP_YYJSON_SOURCE}")
  target_include_directories(sp_yyjson SYSTEM INTERFACE "${SP_YYJSON_INCLUDE_DIR}")
else()
  find_library(SP_YYJSON_LIBRARY yyjson HINTS "${YYJSON_ROOT}/lib" REQUIRED)
  add_library(sp_yyjson UNKNOWN IMPORTED)
  set_target_properties(sp_yyjson PROPERTIES IMPORTED_LOCATION "${SP_YYJSON_LIBRARY}"
                        INTERFACE_INCLUDE_DIRECTORIES "${SP_YYJSON_INCLUDE_DIR}")
endif()

add_library(sp_core INTERFACE)
target_include_directories(sp_core INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/src)

add_library(sp_json STATIC src/json/json.cpp)
target_include_directories(sp_json PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_options(sp_json PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(sp_json PRIVATE sp_yyjson)

add_library(sp_descriptor STATIC src/descriptor/descriptor.cpp)
target_include_directories(sp_descriptor PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_options(sp_descriptor PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(sp_descriptor PRIVATE sp_json)

add_library(sp_codecs STATIC src/core/native.cpp src/codecs/accumulator.cpp
            src/codecs/chat.cpp src/codecs/chat_request.cpp
            src/codecs/messages.cpp src/codecs/messages_request.cpp)
target_compile_options(sp_codecs PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(sp_codecs PUBLIC sp_core PRIVATE sp_json sp_descriptor OpenSSL::Crypto)

add_library(sp_transport STATIC src/transport/http_transport.cpp src/transport/sse_framer.cpp)
target_include_directories(sp_transport
  PUBLIC  ${CMAKE_CURRENT_SOURCE_DIR}/src
  PRIVATE ${SP_ASIO_INCLUDE_DIR})
target_compile_definitions(sp_transport PRIVATE ASIO_STANDALONE ASIO_NO_DEPRECATED)
target_compile_options(sp_transport PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(sp_transport PUBLIC Threads::Threads PRIVATE CURL::libcurl)

add_library(sp_runtime STATIC src/runtime/client.cpp src/runtime/policy.cpp)
target_include_directories(sp_runtime PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_options(sp_runtime PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(sp_runtime PUBLIC sp_core PRIVATE sp_codecs sp_transport sp_descriptor sp_json)

add_library(sp_canary_core STATIC src/canary/profile.cpp src/canary/ledger.cpp
            src/canary/runner.cpp src/canary/report.cpp)
target_include_directories(sp_canary_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_options(sp_canary_core PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(sp_canary_core PRIVATE sp_runtime sp_codecs sp_descriptor sp_json sp_transport)
add_executable(sp_canary tools/canary_main.cpp)
target_compile_options(sp_canary PRIVATE -Wall -Wextra -Wpedantic)
target_link_libraries(sp_canary PRIVATE sp_canary_core)

if(SP_BUILD_TESTS)
  enable_testing()
  # Model-free peers are required for the TLS and HTTP/2 gates, never silently skipped.
  find_program(SP_NODE node REQUIRED)
  find_program(SP_OPENSSL openssl REQUIRED)


  # A positive control separates const-view failures from broken include/toolchain setup.
  function(sp_check_runtime_result)
    set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
    foreach(mutation_case RANGE 0 4)
      try_compile(result_compiles
                  "${CMAKE_CURRENT_BINARY_DIR}/runtime-result-${mutation_case}"
                  "${CMAKE_CURRENT_SOURCE_DIR}/tests/runtime_result_compile.cpp"
                  CMAKE_FLAGS -DCMAKE_CXX_STANDARD=20 -DCMAKE_CXX_STANDARD_REQUIRED=ON
                  COMPILE_DEFINITIONS "-I${CMAKE_CURRENT_SOURCE_DIR}/src"
                                      "-DSP_MUTATION_CASE=${mutation_case}"
                  OUTPUT_VARIABLE compile_output)
      if(mutation_case EQUAL 0 AND NOT result_compiles)
        message(FATAL_ERROR "Runtime result read-only control did not compile: ${compile_output}")
      elseif(mutation_case GREATER 0 AND result_compiles)
        message(FATAL_ERROR "Runtime result mutation ${mutation_case} unexpectedly compiled")
      endif()
    endforeach()
    message(STATUS "Runtime Result: read-only control compiled; four mutations rejected")
  endfunction()
  sp_check_runtime_result()
  add_executable(sp_loopback_server tests/support/loopback_server.cpp)
  target_include_directories(sp_loopback_server PRIVATE ${SP_ASIO_INCLUDE_DIR})
  target_compile_definitions(sp_loopback_server PRIVATE ASIO_STANDALONE ASIO_NO_DEPRECATED)
  target_compile_options(sp_loopback_server PRIVATE -Wall -Wextra -Wpedantic)
  target_link_libraries(sp_loopback_server PRIVATE Threads::Threads)

  add_executable(sp_transport_tests tests/transport_properties_test.cpp)
  target_compile_options(sp_transport_tests PRIVATE -Wall -Wextra -Wpedantic)
  target_link_libraries(sp_transport_tests PRIVATE sp_transport)

  add_executable(sp_sse_tests tests/sse_framer_test.cpp)
  target_link_libraries(sp_sse_tests PRIVATE sp_transport)
  add_executable(sp_descriptor_tests tests/descriptor_properties_test.cpp)
  target_link_libraries(sp_descriptor_tests PRIVATE sp_descriptor sp_json)
  add_executable(sp_chat_tests tests/chat_semantic_test.cpp)
  target_link_libraries(sp_chat_tests PRIVATE sp_codecs sp_descriptor sp_json sp_transport)
  add_executable(sp_messages_tests tests/messages_semantic_test.cpp)
  target_link_libraries(sp_messages_tests PRIVATE sp_codecs sp_descriptor sp_json)
  add_executable(sp_messages_replay_tests tests/messages_replay_test.cpp)
  target_link_libraries(sp_messages_replay_tests PRIVATE sp_codecs sp_descriptor sp_json)
  add_executable(sp_native_continuation_tests tests/native_continuation_bounds_test.cpp)
  target_link_libraries(sp_native_continuation_tests PRIVATE sp_codecs sp_descriptor sp_json)
  add_executable(sp_fixture_runner tests/fixture_runner.cpp)
  target_link_libraries(sp_fixture_runner PRIVATE sp_codecs sp_descriptor sp_json sp_transport)
  add_executable(sp_runtime_policy_tests tests/runtime_policy_test.cpp)
  target_link_libraries(sp_runtime_policy_tests PRIVATE sp_runtime sp_json)
  add_executable(sp_runtime_scheduler_tests tests/runtime_scheduler_test.cpp)
  target_link_libraries(sp_runtime_scheduler_tests PRIVATE sp_runtime sp_json)
  add_executable(sp_runtime_tests tests/runtime_properties_test.cpp)
  target_link_libraries(sp_runtime_tests PRIVATE sp_runtime sp_json)
  add_executable(sp_canary_tests tests/canary_test.cpp)
  target_compile_options(sp_canary_tests PRIVATE -Wall -Wextra -Wpedantic)
  target_link_libraries(sp_canary_tests PRIVATE sp_canary_core sp_json Threads::Threads)
  foreach(target sp_runtime_policy_tests sp_runtime_scheduler_tests sp_runtime_tests)
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
  endforeach()
  foreach(target sp_sse_tests sp_descriptor_tests sp_chat_tests sp_messages_tests sp_messages_replay_tests sp_native_continuation_tests sp_fixture_runner)
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
  endforeach()

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
                   ${CMAKE_CURRENT_SOURCE_DIR}/tests/support)
  set_tests_properties(transport_properties PROPERTIES TIMEOUT 300)
  add_test(NAME sse_properties COMMAND ${sp_test_runner} $<TARGET_FILE:sp_sse_tests>)
  add_test(NAME descriptor_properties COMMAND ${sp_test_runner} $<TARGET_FILE:sp_descriptor_tests>)
  add_test(NAME chat_properties COMMAND ${sp_test_runner} $<TARGET_FILE:sp_chat_tests>)
  add_test(NAME messages_properties COMMAND ${sp_test_runner} $<TARGET_FILE:sp_messages_tests>)
  add_test(NAME messages_replay COMMAND ${sp_test_runner} $<TARGET_FILE:sp_messages_replay_tests>)
  add_test(NAME native_continuation_bounds COMMAND ${sp_test_runner} $<TARGET_FILE:sp_native_continuation_tests>)
  add_test(NAME chat_fixtures COMMAND ${sp_test_runner} $<TARGET_FILE:sp_fixture_runner> ${SP_NODE}
           ${CMAKE_CURRENT_SOURCE_DIR}/tests/support/fixture_server.mjs
           ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/chat
           ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/openai-chat-descriptor.json)
  add_test(NAME messages_fixtures COMMAND ${sp_test_runner} $<TARGET_FILE:sp_fixture_runner> ${SP_NODE}
           ${CMAKE_CURRENT_SOURCE_DIR}/tests/support/fixture_server.mjs
           ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/messages
           ${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/anthropic-messages-descriptor.json)
  add_test(NAME runtime_policy COMMAND ${sp_test_runner} $<TARGET_FILE:sp_runtime_policy_tests>)
  add_test(NAME runtime_scheduler COMMAND ${sp_test_runner} $<TARGET_FILE:sp_runtime_scheduler_tests>
           ${SP_NODE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/support/runtime_server.mjs)
  add_test(NAME runtime_properties COMMAND ${sp_test_runner} $<TARGET_FILE:sp_runtime_tests>
           ${SP_NODE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/support/runtime_server.mjs)
  add_test(NAME canary_properties COMMAND ${sp_test_runner} $<TARGET_FILE:sp_canary_tests>
           ${SP_NODE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/support/canary_server.mjs $<TARGET_FILE:sp_canary>)
  set_tests_properties(canary_properties PROPERTIES TIMEOUT 300)
  set_tests_properties(runtime_policy runtime_scheduler runtime_properties PROPERTIES TIMEOUT 300)
  set_tests_properties(sse_properties descriptor_properties chat_properties chat_fixtures
                       messages_properties messages_replay native_continuation_bounds messages_fixtures PROPERTIES TIMEOUT 300)
endif()

if(SP_BUILD_NEOGRAPH_SPIKE)
  add_subdirectory(integration/neograph)
endif()
