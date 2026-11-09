cmake_minimum_required(VERSION 3.20)
project(sp_install_consumer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(SchemaProvider 0.4.1 EXACT CONFIG REQUIRED)

add_executable(sp_install_consumer main.cpp)
target_link_libraries(sp_install_consumer PRIVATE SchemaProvider::core SchemaProvider::codecs)
target_compile_features(sp_install_consumer PRIVATE cxx_std_20)

enable_testing()
add_test(NAME install_consumer COMMAND sp_install_consumer)
