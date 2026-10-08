cmake_minimum_required(VERSION 3.20)
project(SchemaProvider VERSION 0.1.1 LANGUAGES C CXX)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

set(SP_TOP_LEVEL OFF)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  set(SP_TOP_LEVEL ON)
endif()
option(SP_BUILD_TESTS "Build the property tests, fixture runner and loopback peers" ${SP_TOP_LEVEL})
option(SP_BUILD_CANARY "Build hosted-API qualification diagnostics (POSIX)" ${SP_TOP_LEVEL})
option(SP_BUILD_RUNTIME_TESTS "Build portable native-archive consumer tests" OFF)
if(SP_BUILD_TESTS AND NOT SP_BUILD_CANARY)
  message(FATAL_ERROR "The complete SDK property suite requires SP_BUILD_CANARY=ON")
endif()
option(BUILD_SHARED_LIBS "Build shared SchemaProvider libraries" OFF)
option(SP_BUILD_BENCHMARKS "Build the controlled protocol and native-carry benchmark" OFF)
set(SP_SANITIZE "" CACHE STRING "Sanitizer set: address, thread (undefined is added to address)")
set(SP_HTTP3_PYTHON "" CACHE FILEPATH "Isolated Python with aioquic and h2 for optional real QUIC scenarios")

if(MSVC)
  set(SP_WARNING_OPTIONS /W4)
else()
  set(SP_WARNING_OPTIONS -Wall -Wextra -Wpedantic)
endif()

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
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(SP_GENERATED_INCLUDE_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
  "${CMAKE_CURRENT_SOURCE_DIR}/tools/embed_configuration.py"
  "${CMAKE_CURRENT_SOURCE_DIR}/config/runtime-defaults.json"
  "${CMAKE_CURRENT_SOURCE_DIR}/config/error-policy.json"
  "${CMAKE_CURRENT_SOURCE_DIR}/config/descriptor-policy.json"
  "${CMAKE_CURRENT_SOURCE_DIR}/config/codec-defaults.json"
  "${CMAKE_CURRENT_SOURCE_DIR}/config/model-catalog.json")
execute_process(
  COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/embed_configuration.py"
    --runtime "${CMAKE_CURRENT_SOURCE_DIR}/config/runtime-defaults.json"
    --errors "${CMAKE_CURRENT_SOURCE_DIR}/config/error-policy.json"
    --descriptor-policy "${CMAKE_CURRENT_SOURCE_DIR}/config/descriptor-policy.json"
    --codec-defaults "${CMAKE_CURRENT_SOURCE_DIR}/config/codec-defaults.json"
    --asset model_catalog_json "${CMAKE_CURRENT_SOURCE_DIR}/config/model-catalog.json"
    --output "${SP_GENERATED_INCLUDE_DIR}/sp/config_defaults.h"
  RESULT_VARIABLE SP_CONFIGURATION_RESULT)
if(NOT SP_CONFIGURATION_RESULT EQUAL 0)
  message(FATAL_ERROR "Closed configuration generation failed")
endif()
find_package(CURL 7.88 REQUIRED)
find_package(OpenSSL REQUIRED COMPONENTS Crypto)
find_path(SP_ASIO_INCLUDE_DIR asio.hpp HINTS "${ASIO_ROOT}/include" DOC "standalone Asio headers")
if(NOT SP_ASIO_INCLUDE_DIR)
  message(FATAL_ERROR "standalone Asio not found: install libasio-dev or pass -DASIO_ROOT=<dir containing include/asio.hpp>")
endif()

# yyjson stays private. Use an installed library or an explicitly supplied source tree;
# configuration never downloads dependencies or replaces a system package.
set(YYJSON_ROOT "${YYJSON_ROOT}" CACHE PATH "yyjson installation or source directory")
find_path(SP_YYJSON_INCLUDE_DIR yyjson.h HINTS "${YYJSON_ROOT}" "${YYJSON_ROOT}/src" "${YYJSON_ROOT}/include")
if(NOT SP_YYJSON_INCLUDE_DIR)
  message(FATAL_ERROR "yyjson not found: install yyjson or pass -DYYJSON_ROOT=<source or installation directory>")
endif()
find_file(SP_YYJSON_SOURCE yyjson.c PATHS "${SP_YYJSON_INCLUDE_DIR}" NO_DEFAULT_PATH)
if(SP_YYJSON_SOURCE)
  # Embed the parser objects in sp_json, not an unexported static dependency.
  add_library(sp_yyjson OBJECT "${SP_YYJSON_SOURCE}")
  set_target_properties(sp_yyjson PROPERTIES POSITION_INDEPENDENT_CODE ON)
  target_include_directories(sp_yyjson SYSTEM INTERFACE "${SP_YYJSON_INCLUDE_DIR}")
else()
  find_library(SP_YYJSON_LIBRARY yyjson HINTS "${YYJSON_ROOT}/lib" REQUIRED)
  add_library(sp_yyjson UNKNOWN IMPORTED)
  set_target_properties(sp_yyjson PROPERTIES IMPORTED_LOCATION "${SP_YYJSON_LIBRARY}"
                        INTERFACE_INCLUDE_DIRECTORIES "${SP_YYJSON_INCLUDE_DIR}")
  if(NOT TARGET yyjson::yyjson)
    add_library(yyjson::yyjson UNKNOWN IMPORTED)
    set_target_properties(yyjson::yyjson PROPERTIES
      IMPORTED_LOCATION "${SP_YYJSON_LIBRARY}"
      INTERFACE_INCLUDE_DIRECTORIES "${SP_YYJSON_INCLUDE_DIR}")
  endif()
endif()

add_library(sp_core src/core/image.cpp src/core/native.cpp src/core/native_archive.cpp
                    src/core/native_archive_fs.cpp)
target_compile_options(sp_core PRIVATE ${SP_WARNING_OPTIONS})
target_link_libraries(sp_core PUBLIC sp_json sp_descriptor PRIVATE OpenSSL::Crypto)
if(WIN32)
  target_link_libraries(sp_core PRIVATE advapi32)
endif()

add_library(sp_json src/json/json.cpp)
target_compile_options(sp_json PRIVATE ${SP_WARNING_OPTIONS})
if(SP_YYJSON_SOURCE)
  target_sources(sp_json PRIVATE $<TARGET_OBJECTS:sp_yyjson>)
  target_include_directories(sp_json SYSTEM PRIVATE "${SP_YYJSON_INCLUDE_DIR}")
  set(SP_EXTERNAL_YYJSON OFF)
else()
  target_link_libraries(sp_json PRIVATE yyjson::yyjson)
  set(SP_EXTERNAL_YYJSON ON)
endif()

add_library(sp_descriptor src/descriptor/descriptor.cpp src/descriptor/policy.cpp)
target_compile_options(sp_descriptor PRIVATE ${SP_WARNING_OPTIONS})
target_link_libraries(sp_descriptor PUBLIC sp_json PRIVATE OpenSSL::Crypto)

add_library(sp_codecs src/codecs/accumulator.cpp
            src/codecs/chat.cpp src/codecs/chat_request.cpp
            src/codecs/messages.cpp src/codecs/messages_request.cpp
            src/codecs/responses.cpp src/codecs/responses_request.cpp
            src/codecs/gemini.cpp src/codecs/gemini_request.cpp
            src/codecs/interactions.cpp src/codecs/interactions_request.cpp)
target_compile_options(sp_codecs PRIVATE ${SP_WARNING_OPTIONS})
target_link_libraries(sp_codecs PUBLIC sp_core sp_json sp_descriptor PRIVATE OpenSSL::Crypto)

# Wire types, SSE framing and the I/O-thread marker: everything the runtime client needs from the
# transport without libcurl.
add_library(sp_wire src/transport/sse_framer.cpp src/transport/io_thread.cpp)
target_compile_options(sp_wire PRIVATE ${SP_WARNING_OPTIONS})
# The libcurl backend and the public Client constructor that uses it.
add_library(sp_transport src/transport/http_transport.cpp src/transport/real_attempt_transport.cpp)
target_include_directories(sp_transport SYSTEM PRIVATE "${SP_ASIO_INCLUDE_DIR}")
target_compile_definitions(sp_transport PRIVATE ASIO_STANDALONE ASIO_NO_DEPRECATED)
target_compile_options(sp_transport PRIVATE ${SP_WARNING_OPTIONS})
target_link_libraries(sp_transport PUBLIC sp_runtime sp_wire sp_core Threads::Threads PRIVATE CURL::libcurl)
if(WIN32)
  target_link_libraries(sp_transport PRIVATE ws2_32)
  target_compile_definitions(sp_transport PRIVATE _WIN32_WINNT=0x0601)
endif()

add_library(sp_runtime src/runtime/client.cpp src/runtime/policy.cpp src/configuration/runtime_policy.cpp)
target_compile_options(sp_runtime PRIVATE ${SP_WARNING_OPTIONS})
target_link_libraries(sp_runtime PUBLIC sp_core sp_codecs sp_wire sp_descriptor sp_json PRIVATE OpenSSL::Crypto)

if(APPLE)
  set(SP_LIBRARY_INSTALL_RPATH "@loader_path")
elseif(UNIX)
  set(SP_LIBRARY_INSTALL_RPATH "$ORIGIN")
endif()

foreach(component core json descriptor codecs wire transport runtime)
  add_library(SchemaProvider::${component} ALIAS sp_${component})
  target_include_directories(sp_${component} PUBLIC
    "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/src>"
    "$<BUILD_INTERFACE:${SP_GENERATED_INCLUDE_DIR}>"
    "$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/SchemaProvider>")
  target_compile_features(sp_${component} PUBLIC cxx_std_20)
  set_target_properties(sp_${component} PROPERTIES
    EXPORT_NAME ${component}
    POSITION_INDEPENDENT_CODE ON
    SOVERSION 4
    INSTALL_RPATH "${SP_LIBRARY_INSTALL_RPATH}"
    WINDOWS_EXPORT_ALL_SYMBOLS ON)
endforeach()

include(cmake/SchemaProviderInstall.cmake)

if(SP_BUILD_CANARY)
add_library(sp_qualification STATIC src/qualification/catalog.cpp src/qualification/meter.cpp)
target_include_directories(sp_qualification PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src "${SP_GENERATED_INCLUDE_DIR}")
target_compile_options(sp_qualification PRIVATE ${SP_WARNING_OPTIONS})
target_link_libraries(sp_qualification PRIVATE sp_json OpenSSL::Crypto)

add_library(sp_canary_core STATIC src/canary/profile.cpp src/canary/ledger.cpp
            src/canary/runner.cpp src/canary/vision.cpp src/canary/report.cpp)
target_include_directories(sp_canary_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_options(sp_canary_core PRIVATE ${SP_WARNING_OPTIONS})
target_link_libraries(sp_canary_core PUBLIC sp_core PRIVATE sp_runtime sp_codecs sp_descriptor sp_json sp_transport sp_qualification OpenSSL::Crypto)
add_executable(sp_canary tools/canary_main.cpp)
target_compile_options(sp_canary PRIVATE ${SP_WARNING_OPTIONS})
target_link_libraries(sp_canary PRIVATE sp_canary_core)
endif()

if(SP_BUILD_TESTS)
  enable_testing()
  # Model-free peers are required for the TLS and HTTP/2 gates, never silently skipped.
  find_program(SP_NODE node REQUIRED)
  find_program(SP_OPENSSL openssl REQUIRED)
  # ThreadSanitizer's shadow memory needs a fixed address-space layout; kernels with high mmap
  # entropy (this WSL2 kernel) abort with "unexpected memory mapping" unless ASLR is off.
  set(sp_test_runner "")
  if(SP_SANITIZE STREQUAL "thread")
    find_program(SP_SETARCH setarch)
    if(SP_SETARCH)
      set(sp_test_runner ${SP_SETARCH} ${CMAKE_SYSTEM_PROCESSOR} -R)
    endif()
  endif()
  add_executable(sp_qualification_meter_test tests/qualification_meter_test.cpp)
  target_compile_options(sp_qualification_meter_test PRIVATE ${SP_WARNING_OPTIONS})
  target_link_libraries(sp_qualification_meter_test PRIVATE sp_qualification sp_json OpenSSL::Crypto Threads::Threads)
  add_test(NAME qualification_meter COMMAND ${sp_test_runner} $<TARGET_FILE:sp_qualification_meter_test> "${CMAKE_CURRENT_SOURCE_DIR}")
  set_tests_properties(qualification_meter PROPERTIES TIMEOUT 120)
  add_executable(sp_native_archive_tests tests/native_archive_test.cpp)
  target_compile_options(sp_native_archive_tests PRIVATE ${SP_WARNING_OPTIONS})
  target_link_libraries(sp_native_archive_tests PRIVATE sp_core sp_codecs OpenSSL::Crypto Threads::Threads)
  add_test(NAME native_archive COMMAND ${sp_test_runner} $<TARGET_FILE:sp_native_archive_tests>)
  set_tests_properties(native_archive PROPERTIES TIMEOUT 180)


  # A positive control separates const-view failures from broken include/toolchain setup.
  function(sp_check_runtime_result)
    set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
    foreach(mutation_case RANGE 0 4)
      try_compile(result_compiles
                  "${CMAKE_CURRENT_BINARY_DIR}/runtime-result-${mutation_case}"
                  "${CMAKE_CURRENT_SOURCE_DIR}/tests/runtime_result_compile.cpp"
                  CMAKE_FLAGS -DCMAKE_CXX_STANDARD=20 -DCMAKE_CXX_STANDARD_REQUIRED=ON
                  COMPILE_DEFINITIONS "-I${CMAKE_CURRENT_SOURCE_DIR}/src"
                                      "-I${SP_GENERATED_INCLUDE_DIR}"
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
  target_compile_options(sp_loopback_server PRIVATE ${SP_WARNING_OPTIONS})
  target_link_libraries(sp_loopback_server PRIVATE Threads::Threads)

  add_executable(sp_transport_tests tests/transport_properties_test.cpp)
  target_compile_options(sp_transport_tests PRIVATE ${SP_WARNING_OPTIONS})
  target_link_libraries(sp_transport_tests PRIVATE sp_transport)
  add_executable(sp_http3_scenarios tests/http3_transport_scenarios.cpp)
  target_compile_options(sp_http3_scenarios PRIVATE ${SP_WARNING_OPTIONS})
  target_link_libraries(sp_http3_scenarios PRIVATE sp_transport)
  if(SP_HTTP3_PYTHON)
    add_test(NAME http3_scenarios COMMAND ${sp_test_runner} $<TARGET_FILE:sp_http3_scenarios>
      "${SP_HTTP3_PYTHON}" "${CMAKE_CURRENT_SOURCE_DIR}/tests/support/http3_peer.py" all)
    set_tests_properties(http3_scenarios PROPERTIES TIMEOUT 180)
  endif()

  add_executable(sp_sse_tests tests/sse_framer_test.cpp)
  target_link_libraries(sp_sse_tests PRIVATE sp_transport)
  add_executable(sp_descriptor_tests tests/descriptor_properties_test.cpp)
  target_link_libraries(sp_descriptor_tests PRIVATE sp_descriptor sp_json)
  add_executable(sp_chat_tests tests/chat_semantic_test.cpp)
  target_link_libraries(sp_chat_tests PRIVATE sp_codecs sp_descriptor sp_json sp_transport)
  add_executable(sp_stop_reasoning_preservation_tests tests/stop_reasoning_preservation_test.cpp)
  target_link_libraries(sp_stop_reasoning_preservation_tests PRIVATE sp_codecs sp_descriptor sp_json sp_transport)
  target_compile_options(sp_stop_reasoning_preservation_tests PRIVATE ${SP_WARNING_OPTIONS})
  add_executable(sp_messages_tests tests/messages_semantic_test.cpp)
  target_link_libraries(sp_messages_tests PRIVATE sp_codecs sp_descriptor sp_json)
  add_executable(sp_messages_replay_tests tests/messages_replay_test.cpp)
  target_link_libraries(sp_messages_replay_tests PRIVATE sp_codecs sp_descriptor sp_json)
  add_executable(sp_native_continuation_tests tests/native_continuation_bounds_test.cpp)
  target_link_libraries(sp_native_continuation_tests PRIVATE sp_codecs sp_descriptor sp_json)
  add_executable(sp_responses_tests tests/responses_semantic_test.cpp)
  target_link_libraries(sp_responses_tests PRIVATE sp_codecs sp_descriptor sp_json sp_transport)
  target_compile_options(sp_responses_tests PRIVATE ${SP_WARNING_OPTIONS})
  add_executable(sp_responses_runtime_tests tests/responses_runtime_test.cpp)
  target_link_libraries(sp_responses_runtime_tests PRIVATE sp_runtime sp_codecs sp_descriptor sp_json sp_transport)
  target_compile_options(sp_responses_runtime_tests PRIVATE ${SP_WARNING_OPTIONS})
  add_executable(sp_vision_inputs_tests tests/vision_inputs_test.cpp)
  target_link_libraries(sp_vision_inputs_tests PRIVATE sp_codecs sp_descriptor sp_json)
  target_compile_options(sp_vision_inputs_tests PRIVATE ${SP_WARNING_OPTIONS})
  add_executable(sp_gemini_tests tests/gemini_semantic_test.cpp)
  target_link_libraries(sp_gemini_tests PRIVATE sp_codecs sp_descriptor sp_json)
  add_executable(sp_gemini_runtime_tests tests/gemini_runtime_test.cpp)
  target_link_libraries(sp_gemini_runtime_tests PRIVATE sp_runtime sp_transport sp_codecs sp_descriptor sp_json)
  add_executable(sp_interactions_tests tests/interactions_semantic_test.cpp)
  target_link_libraries(sp_interactions_tests PRIVATE sp_codecs sp_descriptor sp_json)
  add_executable(sp_interactions_runtime_tests tests/interactions_runtime_test.cpp)
  target_link_libraries(sp_interactions_runtime_tests PRIVATE sp_runtime sp_transport sp_codecs sp_descriptor sp_json)
  add_executable(sp_vision_campaign_tests tests/vision_campaign_test.cpp)
  target_link_libraries(sp_vision_campaign_tests PRIVATE sp_runtime sp_transport sp_codecs sp_descriptor sp_json)
  foreach(target sp_gemini_tests sp_gemini_runtime_tests sp_interactions_tests sp_interactions_runtime_tests sp_vision_campaign_tests)
    target_compile_options(${target} PRIVATE ${SP_WARNING_OPTIONS})
  endforeach()
  add_executable(sp_fixture_runner tests/fixture_runner.cpp)
  target_link_libraries(sp_fixture_runner PRIVATE sp_codecs sp_descriptor sp_json sp_transport)
  add_executable(sp_runtime_policy_tests tests/runtime_policy_test.cpp)
  target_link_libraries(sp_runtime_policy_tests PRIVATE sp_runtime sp_json)
  add_executable(sp_runtime_scheduler_tests tests/runtime_scheduler_test.cpp)
  target_link_libraries(sp_runtime_scheduler_tests PRIVATE sp_runtime sp_transport sp_json)
  add_executable(sp_runtime_tests tests/runtime_properties_test.cpp)
  target_link_libraries(sp_runtime_tests PRIVATE sp_runtime sp_transport sp_json)
  if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_executable(sp_runtime_footprint_probe tests/runtime_footprint_probe.cpp)
    target_link_libraries(sp_runtime_footprint_probe PRIVATE sp_runtime)
    add_test(NAME runtime_links_no_libcurl COMMAND sp_runtime_footprint_probe)
  endif()
  add_executable(sp_canary_tests tests/canary_test.cpp)
  target_compile_options(sp_canary_tests PRIVATE ${SP_WARNING_OPTIONS})
  target_link_libraries(sp_canary_tests PRIVATE sp_canary_core sp_json Threads::Threads)
  add_executable(sp_vision_canary_tests tests/vision_canary_test.cpp)
  target_compile_options(sp_vision_canary_tests PRIVATE ${SP_WARNING_OPTIONS})
  target_link_libraries(sp_vision_canary_tests PRIVATE sp_canary_core sp_json Threads::Threads)
  add_executable(sp_vision_control_runtime_tests tests/vision_control_runtime_test.cpp)
  target_compile_options(sp_vision_control_runtime_tests PRIVATE ${SP_WARNING_OPTIONS})
  target_link_libraries(sp_vision_control_runtime_tests PRIVATE sp_canary_core sp_json)
  foreach(target sp_runtime_policy_tests sp_runtime_scheduler_tests sp_runtime_tests)
    target_compile_options(${target} PRIVATE ${SP_WARNING_OPTIONS})
  endforeach()
  foreach(target sp_sse_tests sp_descriptor_tests sp_chat_tests sp_messages_tests sp_messages_replay_tests sp_native_continuation_tests sp_fixture_runner)
    target_compile_options(${target} PRIVATE ${SP_WARNING_OPTIONS})
  endforeach()


  add_test(NAME transport_properties
           COMMAND ${sp_test_runner} $<TARGET_FILE:sp_transport_tests> $<TARGET_FILE:sp_loopback_server>
                   ${CMAKE_CURRENT_SOURCE_DIR}/tests/support)
  set_tests_properties(transport_properties PROPERTIES TIMEOUT 300)
  add_test(NAME sse_properties COMMAND ${sp_test_runner} $<TARGET_FILE:sp_sse_tests>)
  add_test(NAME descriptor_properties COMMAND ${sp_test_runner} $<TARGET_FILE:sp_descriptor_tests>)
  add_test(NAME chat_properties COMMAND ${sp_test_runner} $<TARGET_FILE:sp_chat_tests>)
  add_test(NAME stop_reasoning_preservation COMMAND ${sp_test_runner} $<TARGET_FILE:sp_stop_reasoning_preservation_tests>)
  add_test(NAME messages_properties COMMAND ${sp_test_runner} $<TARGET_FILE:sp_messages_tests>)
  add_test(NAME messages_replay COMMAND ${sp_test_runner} $<TARGET_FILE:sp_messages_replay_tests>)
  add_test(NAME native_continuation_bounds COMMAND ${sp_test_runner} $<TARGET_FILE:sp_native_continuation_tests>)
  add_test(NAME responses_properties COMMAND ${sp_test_runner} $<TARGET_FILE:sp_responses_tests>)
  add_test(NAME responses_runtime COMMAND ${sp_test_runner} $<TARGET_FILE:sp_responses_runtime_tests>
           ${SP_NODE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/support/responses_server.mjs)
  set_tests_properties(responses_properties responses_runtime PROPERTIES TIMEOUT 300)
  add_test(NAME vision_inputs COMMAND ${sp_test_runner} $<TARGET_FILE:sp_vision_inputs_tests>)
  add_test(NAME gemini_properties COMMAND ${sp_test_runner} $<TARGET_FILE:sp_gemini_tests>)
  add_test(NAME gemini_runtime COMMAND ${sp_test_runner} $<TARGET_FILE:sp_gemini_runtime_tests>
           ${SP_NODE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/support/gemini_server.mjs)
  add_test(NAME interactions_properties COMMAND ${sp_test_runner} $<TARGET_FILE:sp_interactions_tests>)
  add_test(NAME interactions_runtime COMMAND ${sp_test_runner} $<TARGET_FILE:sp_interactions_runtime_tests>
           ${SP_NODE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/support/interactions_server.mjs)
  add_test(NAME vision_campaign COMMAND ${sp_test_runner} $<TARGET_FILE:sp_vision_campaign_tests>
           ${SP_NODE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/support/vision_server.mjs)
  set_tests_properties(vision_inputs gemini_properties gemini_runtime interactions_properties interactions_runtime vision_campaign PROPERTIES TIMEOUT 300)
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
  add_test(NAME vision_canary COMMAND ${sp_test_runner} $<TARGET_FILE:sp_vision_canary_tests>
           ${SP_NODE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/support/vision_server.mjs $<TARGET_FILE:sp_canary>)
  set_tests_properties(vision_canary PROPERTIES TIMEOUT 300)
  add_test(NAME vision_control_runtime COMMAND ${sp_test_runner} $<TARGET_FILE:sp_vision_control_runtime_tests>
           ${SP_NODE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/support/vision_server.mjs $<TARGET_FILE:sp_canary>)
  set_tests_properties(vision_control_runtime PROPERTIES TIMEOUT 300)
  set_tests_properties(runtime_policy runtime_scheduler runtime_properties PROPERTIES TIMEOUT 300)
  set_tests_properties(sse_properties descriptor_properties chat_properties chat_fixtures
                       messages_properties messages_replay native_continuation_bounds messages_fixtures PROPERTIES TIMEOUT 300)
endif()

if(SP_BUILD_RUNTIME_TESTS)
  enable_testing()
  add_executable(sp_native_archive_portability_tests tests/native_archive_portability_test.cpp)
  target_compile_options(sp_native_archive_portability_tests PRIVATE ${SP_WARNING_OPTIONS})
  target_link_libraries(sp_native_archive_portability_tests PRIVATE sp_codecs Threads::Threads)
  if(WIN32)
    target_link_libraries(sp_native_archive_portability_tests PRIVATE advapi32)
  endif()
  add_test(NAME native_archive_portability COMMAND sp_native_archive_portability_tests)
  set_tests_properties(native_archive_portability PROPERTIES TIMEOUT 120)
endif()


if(SP_BUILD_BENCHMARKS)
  add_subdirectory(benchmarks)
endif()
