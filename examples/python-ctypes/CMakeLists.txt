cmake_minimum_required(VERSION 3.20)
project(sp_capi_example LANGUAGES CXX)

# An installed SchemaProvider (see the README install recipe) and the header-only cppdotenv.
find_package(SchemaProvider 0.3.0 EXACT CONFIG REQUIRED)
find_package(cppdotenv CONFIG QUIET)
if(NOT cppdotenv_FOUND)
  include(FetchContent)
  FetchContent_Declare(cppdotenv GIT_REPOSITORY https://github.com/fox1245/cppdotenv.git GIT_TAG d7cfb36922ce72c096913dab2be369ec4db28d82)
  set(CPPDOTENV_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(CPPDOTENV_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(cppdotenv)
endif()

# With a static, PIC SDK installation, this links the SDK into the example shared library.
# Only the functions in sp_capi.h belong to this example's C ABI.
add_library(sp_capi SHARED sp_capi.cpp)
target_compile_features(sp_capi PRIVATE cxx_std_20)
set_target_properties(sp_capi PROPERTIES CXX_VISIBILITY_PRESET hidden VISIBILITY_INLINES_HIDDEN ON)
target_link_libraries(sp_capi PRIVATE SchemaProvider::transport cppdotenv::cppdotenv)
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  # Keep all C++/RTTI symbols private, including weak definitions emitted by this shim.
  file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/sp_capi.map" CONTENT
"{ global: sp_capi_interface_revision; sp_capi_string_free; sp_capi_client_create; sp_capi_client_destroy; sp_capi_conversation_create; sp_capi_conversation_destroy; sp_capi_conversation_send; local: *; };\n")
  target_link_options(sp_capi PRIVATE -Wl,--exclude-libs,ALL "LINKER:--version-script,${CMAKE_CURRENT_BINARY_DIR}/sp_capi.map")
elseif(APPLE)
  file(GENERATE OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/sp_capi.exports" CONTENT
"_sp_capi_interface_revision\n_sp_capi_string_free\n_sp_capi_client_create\n_sp_capi_client_destroy\n_sp_capi_conversation_create\n_sp_capi_conversation_destroy\n_sp_capi_conversation_send\n")
  target_link_options(sp_capi PRIVATE "LINKER:-exported_symbols_list,${CMAKE_CURRENT_BINARY_DIR}/sp_capi.exports")
endif()
