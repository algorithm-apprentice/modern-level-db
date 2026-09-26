find_package(Python3 3.9 REQUIRED COMPONENTS Interpreter)
if(MODERN_LEVELDB_BUILD_BENCHMARKS)
  add_executable(modern_leveldb_bench db_bench.cc)
  target_include_directories(modern_leveldb_bench PRIVATE "${PROJECT_SOURCE_DIR}/tests")
  target_link_libraries(modern_leveldb_bench PRIVATE modern_leveldb::modern_leveldb leveldb)
  modern_leveldb_set_project_warnings(modern_leveldb_bench PRIVATE)

  add_test(
    NAME benchmark.checker
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/tools/benchmark_gate_test.py"
  )
  set_tests_properties(benchmark.checker PROPERTIES LABELS benchmark TIMEOUT 10)
  add_test(
    NAME benchmark.severe_regression
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/check_benchmark.py"
      "${CMAKE_CURRENT_BINARY_DIR}/results.json" --run "$<TARGET_FILE:modern_leveldb_bench>"
  )
  set_tests_properties(benchmark.severe_regression PROPERTIES LABELS benchmark TIMEOUT 240)
endif()

if(MODERN_LEVELDB_BUILD_PERFORMANCE_TESTS)
  execute_process(
    COMMAND git -C "${PROJECT_SOURCE_DIR}" rev-parse HEAD
    RESULT_VARIABLE revision_status
    OUTPUT_VARIABLE PROFILE_CONFIGURE_REVISION
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
  )
  execute_process(
    COMMAND git -C "${PROJECT_SOURCE_DIR}" status --porcelain --untracked-files=normal
    RESULT_VARIABLE dirty_status
    OUTPUT_VARIABLE source_changes
    ERROR_QUIET
  )
  set(PROFILE_CONFIGURE_DIRTY "unknown")
  if(revision_status EQUAL 0 AND dirty_status EQUAL 0)
    if(source_changes STREQUAL "")
      set(PROFILE_CONFIGURE_DIRTY "false")
    else()
      set(PROFILE_CONFIGURE_DIRTY "true")
    endif()
  else()
    set(PROFILE_CONFIGURE_REVISION "unavailable")
    message(STATUS "Profiling source revision is unavailable in this checkout")
  endif()
  string(TOUPPER "${CMAKE_BUILD_TYPE}" profile_config)
  set(PROFILE_C_FLAGS "${CMAKE_C_FLAGS} ${CMAKE_C_FLAGS_${profile_config}}")
  set(PROFILE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${CMAKE_CXX_FLAGS_${profile_config}}")
  modern_leveldb_profile_markers(MODERN_LEVELDB_PROFILE_MARKERS)
  configure_file(profiling_build.h.in profiling_build.h @ONLY)

  add_executable(modern_leveldb_performance profiling_bench.cc)
  target_include_directories(modern_leveldb_performance PRIVATE "${CMAKE_CURRENT_BINARY_DIR}")
  target_link_libraries(
    modern_leveldb_performance PRIVATE modern_leveldb::modern_leveldb leveldb benchmark::benchmark
  )
  modern_leveldb_set_project_warnings(modern_leveldb_performance PRIVATE)
  add_test(
    NAME performance.contracts
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/tools/performance_test.py"
      --binary "$<TARGET_FILE:modern_leveldb_performance>"
  )
  add_test(
    NAME performance.profile_parser
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/tools/profile_report_test.py"
  )
  add_test(
    NAME performance.process_control
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/tools/profile_process_test.py"
  )
  add_test(
    NAME performance.smoke
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/tools/performance_smoke.py"
      --binary "$<TARGET_FILE:modern_leveldb_performance>"
      --output "${CMAKE_CURRENT_BINARY_DIR}/performance-smoke"
  )
  set_tests_properties(
    performance.contracts performance.profile_parser performance.process_control
    PROPERTIES LABELS performance TIMEOUT 120
  )
  set_tests_properties(performance.smoke PROPERTIES LABELS performance TIMEOUT 600)
  add_test(
    NAME performance.capture_capabilities
    COMMAND "${CMAKE_COMMAND}" -P "${PROJECT_SOURCE_DIR}/tests/cmake/profile_capabilities.cmake"
  )
  add_test(
    NAME performance.dependency_declaration
    COMMAND "${CMAKE_COMMAND}"
      "-DCONSUMER_SOURCE_DIR=${PROJECT_SOURCE_DIR}/tests/cmake/benchmark_declaration"
      "-DBINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}/benchmark-declaration"
      "-DGENERATOR=${CMAKE_GENERATOR}" "-DCOMPILER=${CMAKE_CXX_COMPILER}"
      "-DSOURCE_DIR=${PROJECT_SOURCE_DIR}"
      "-DEXPECTED_ERROR=predeclared Google Benchmark dependency"
      -P "${PROJECT_SOURCE_DIR}/tests/cmake/reject_reference.cmake"
  )
  set_tests_properties(
    performance.capture_capabilities performance.dependency_declaration
    PROPERTIES LABELS performance TIMEOUT 120
  )
endif()
