# The parent project opts in with add_subdirectory(benchmarks).
add_executable(sp_provider_benchmark provider_benchmark.cpp)
target_compile_features(sp_provider_benchmark PRIVATE cxx_std_20)
target_include_directories(sp_provider_benchmark PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../tests/support")
target_link_libraries(sp_provider_benchmark PRIVATE sp_runtime sp_codecs sp_descriptor sp_json sp_transport Threads::Threads)
target_compile_definitions(sp_provider_benchmark PRIVATE SP_BENCHMARK_BUILD_TYPE="${CMAKE_BUILD_TYPE}")
