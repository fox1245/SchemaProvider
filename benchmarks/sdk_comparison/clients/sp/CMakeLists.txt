# The SchemaProvider client program of the SDK comparison. Included from the top-level CMakeLists.txt when
# SP_BUILD_SDK_COMPARISON is ON.
add_executable(sdkb_sp_client sp_client.cpp)
target_compile_features(sdkb_sp_client PRIVATE cxx_std_20)
target_link_libraries(sdkb_sp_client PRIVATE sp_runtime sp_transport sp_descriptor sp_json Threads::Threads)
find_package(Git QUIET)
if(NOT SDKB_SP_REVISION)
  set(SDKB_SP_REVISION "unknown")
endif()
if(GIT_FOUND AND SDKB_SP_REVISION STREQUAL "unknown")
  execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${PROJECT_SOURCE_DIR}" rev-parse --short=12 HEAD
                  OUTPUT_VARIABLE SDKB_SP_REVISION OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
endif()
target_compile_definitions(sdkb_sp_client PRIVATE SDKB_CLIENT_VERSION="${PROJECT_VERSION}+${SDKB_SP_REVISION}")
