# The ai-sdk-cpp client program of the SDK comparison. Configure with
#   -DAI_SDK_CPP_SOURCE=<checkout> -DZLIB_SOURCE=<zlib checkout> -DAI_SDK_CPP_REVISION=<sha>
cmake_minimum_required(VERSION 3.20)
project(sdkb_ai_sdk_cpp LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
# The SDK's CMake would download zlib; use the pinned checkout instead.
set(FETCHCONTENT_SOURCE_DIR_ZLIB "${ZLIB_SOURCE}" CACHE PATH "" FORCE)
set(FETCHCONTENT_FULLY_DISCONNECTED ON CACHE BOOL "" FORCE)
find_package(Threads REQUIRED)
add_subdirectory("${AI_SDK_CPP_SOURCE}" ai-sdk-cpp EXCLUDE_FROM_ALL)
add_executable(sdkb_ai_sdk_cpp_client ai_client.cpp)
target_link_libraries(sdkb_ai_sdk_cpp_client PRIVATE ai::openai ai::anthropic ai::core Threads::Threads)
target_compile_definitions(sdkb_ai_sdk_cpp_client PRIVATE SDKB_CLIENT_VERSION="0.1.0+${AI_SDK_CPP_REVISION}")
