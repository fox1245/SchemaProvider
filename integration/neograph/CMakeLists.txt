# Included only by the parent's opt-in add_subdirectory(integration/neograph).
# Never add_subdirectory() or configure the NeoGraph checkout itself.
set(SP_NEOGRAPH_SOURCE "" CACHE PATH "Read-only current NeoGraph source checkout")
if(NOT EXISTS "${SP_NEOGRAPH_SOURCE}/include/neograph/completion_provider.h")
  message(FATAL_ERROR "Set SP_NEOGRAPH_SOURCE to the current read-only NeoGraph checkout")
endif()
find_package(OpenSSL REQUIRED COMPONENTS SSL Crypto)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
find_program(SP_NODE node REQUIRED)
set(sp_ng_generated "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${sp_ng_generated}")
file(GLOB sp_ng_schemas CONFIGURE_DEPENDS "${SP_NEOGRAPH_SOURCE}/schemas/*.json")
add_custom_command(
  OUTPUT "${sp_ng_generated}/builtin_schemas.h"
  COMMAND Python3::Interpreter "${SP_NEOGRAPH_SOURCE}/scripts/embed_schemas.py"
          "${SP_NEOGRAPH_SOURCE}/schemas" "${sp_ng_generated}/builtin_schemas.h"
  DEPENDS "${SP_NEOGRAPH_SOURCE}/scripts/embed_schemas.py" ${sp_ng_schemas}
  VERBATIM)
# Compile the selected, unchanged actual implementation, not a parser facsimile.
add_library(sp_neograph_legacy STATIC
  "${SP_NEOGRAPH_SOURCE}/src/core/json.cpp"
  "${SP_NEOGRAPH_SOURCE}/src/core/cancel.cpp"
  "${SP_NEOGRAPH_SOURCE}/src/core/provider.cpp"
  "${SP_NEOGRAPH_SOURCE}/src/core/completion_provider.cpp"
  "${SP_NEOGRAPH_SOURCE}/src/async/http_client.cpp"
  "${SP_NEOGRAPH_SOURCE}/src/async/conn_pool.cpp"
  "${SP_NEOGRAPH_SOURCE}/src/async/curl_h2_pool.cpp"
  "${SP_NEOGRAPH_SOURCE}/src/async/sse_parser.cpp"
  "${SP_NEOGRAPH_SOURCE}/src/async/ws_client.cpp"
  "${SP_NEOGRAPH_SOURCE}/src/llm/schema_strategy_registry.cpp"
  "${SP_NEOGRAPH_SOURCE}/src/llm/schema_primitive_registry.cpp"
  "${SP_NEOGRAPH_SOURCE}/src/llm/schema_provider.cpp"
  "${SP_NEOGRAPH_SOURCE}/src/llm/schema_provider_mapping.cpp"
  "${SP_NEOGRAPH_SOURCE}/src/llm/schema_provider_stream_parser.cpp"
  "${sp_ng_generated}/builtin_schemas.h")
target_include_directories(sp_neograph_legacy PUBLIC
  "${SP_NEOGRAPH_SOURCE}/include" "${SP_ASIO_INCLUDE_DIR}" "${SP_YYJSON_INCLUDE_DIR}"
  PRIVATE "${SP_NEOGRAPH_SOURCE}/deps" "${sp_ng_generated}")
target_compile_definitions(sp_neograph_legacy PUBLIC NEOGRAPH_STATIC_BUILD ASIO_STANDALONE ASIO_NO_DEPRECATED)
target_link_libraries(sp_neograph_legacy PUBLIC sp_yyjson Threads::Threads OpenSSL::SSL OpenSSL::Crypto)
# curl_h2_pool.cpp provides its unchanged disabled-backend implementation; the
# legacy fixture driver uses its current Asio buffered + httplib SSE defaults.
add_library(sp_neograph_adapter STATIC adapter.cpp)
target_include_directories(sp_neograph_adapter PUBLIC "${PROJECT_SOURCE_DIR}")
target_link_libraries(sp_neograph_adapter PUBLIC sp_neograph_legacy sp_runtime PRIVATE sp_json)
target_compile_options(sp_neograph_adapter PRIVATE -Wall -Wextra -Wpedantic)
add_executable(sp_neograph_legacy_case legacy_case.cpp)
target_link_libraries(sp_neograph_legacy_case PRIVATE sp_neograph_legacy)
target_include_directories(sp_neograph_legacy_case PRIVATE "${PROJECT_SOURCE_DIR}" "${sp_ng_generated}")
if(SP_BUILD_TESTS)
  add_executable(sp_neograph_adapter_tests "${PROJECT_SOURCE_DIR}/tests/neograph_adapter_test.cpp")
  target_link_libraries(sp_neograph_adapter_tests PRIVATE sp_neograph_adapter sp_json)
  add_test(NAME neograph_adapter_boundary
    COMMAND ${sp_test_runner} $<TARGET_FILE:sp_neograph_adapter_tests> ${SP_NODE}
            "${PROJECT_SOURCE_DIR}/tests/neograph_peer.mjs")
  set_tests_properties(neograph_adapter_boundary PROPERTIES TIMEOUT 180)
endif()
# Differential is intentionally NOT a passing CTest alias: exit 1 means observed
# mismatches/representation restrictions. Its JSONL summary is the no-go input.
add_custom_target(sp_neograph_differential
  COMMAND "${SP_NODE}" "${CMAKE_CURRENT_SOURCE_DIR}/differential.mjs"
          $<TARGET_FILE:sp_neograph_legacy_case> "${PROJECT_SOURCE_DIR}/tests/fixtures"
  DEPENDS sp_neograph_legacy_case USES_TERMINAL VERBATIM)
