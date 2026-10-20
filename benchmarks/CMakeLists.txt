# The parent project opts in with add_subdirectory(benchmarks).
add_executable(sp_provider_benchmark provider_benchmark.cpp)
target_compile_features(sp_provider_benchmark PRIVATE cxx_std_20)
target_include_directories(sp_provider_benchmark PRIVATE "${CMAKE_CURRENT_LIST_DIR}/../tests/support")
target_link_libraries(sp_provider_benchmark PRIVATE sp_runtime sp_codecs sp_descriptor sp_json sp_transport Threads::Threads)
target_compile_definitions(sp_provider_benchmark PRIVATE SP_BENCHMARK_BUILD_TYPE="${CMAKE_BUILD_TYPE}")

# A model-free smoke run: one tiny workload per family, so the policy and catalogue the benchmark
# builds cannot go stale unnoticed. It takes no timing.
if(SP_BUILD_TESTS)
  foreach(spec openai.chat:text:runtime anthropic.messages:text:runtime openai.responses:text:runtime
               google.generate:text:runtime google.interactions:text:runtime
               openai.chat:native:runtime openai.chat:native:encode)
    string(REPLACE ":" ";" parts "${spec}")
    list(GET parts 0 family)
    list(GET parts 1 case)
    list(GET parts 2 mode)
    set(iterations 0)
    if(mode STREQUAL "encode")
      set(iterations 10)
    endif()
    string(REPLACE "." "_" name "${family}_${case}_${mode}")
    configure_file("${CMAKE_CURRENT_LIST_DIR}/smoke.json.in" "${CMAKE_CURRENT_BINARY_DIR}/smoke/${name}.json" @ONLY)
    add_test(NAME provider_benchmark_${name} COMMAND sp_provider_benchmark "${CMAKE_CURRENT_BINARY_DIR}/smoke/${name}.json"
             WORKING_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}/..")
    set_tests_properties(provider_benchmark_${name} PROPERTIES TIMEOUT 120)
  endforeach()
endif()
