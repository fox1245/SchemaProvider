cmake_minimum_required(VERSION 3.20)
project(sp_chat_standalone LANGUAGES CXX)

find_package(SchemaProvider 0.3.0 EXACT CONFIG REQUIRED)

include(FetchContent)
FetchContent_Declare(cppdotenv
  GIT_REPOSITORY https://github.com/fox1245/cppdotenv.git
  GIT_TAG d7cfb36922ce72c096913dab2be369ec4db28d82
)
set(CPPDOTENV_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(CPPDOTENV_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
# Offline builds may set FETCHCONTENT_SOURCE_DIR_CPPDOTENV to the pinned local checkout.
FetchContent_MakeAvailable(cppdotenv)

add_executable(sp_chat_standalone main.cpp)
target_compile_features(sp_chat_standalone PRIVATE cxx_std_20)
set_target_properties(sp_chat_standalone PROPERTIES CXX_EXTENSIONS OFF)
target_link_libraries(sp_chat_standalone PRIVATE SchemaProvider::transport cppdotenv::cppdotenv)
