find_package(Python3 3.9 REQUIRED COMPONENTS Interpreter)
if(MODERN_LEVELDB_BUILD_BENCHMARKS)
  set(benchmark_policy_arguments)
  if(MODERN_LEVELDB_HAVE_WINDOWS_FILE_SYSTEM)
    set(BENCHMARK_TARGET_ARCHITECTURE "native_x64")
    set(BENCHMARK_MODERN_FILE_ACCESS "copied")
    set(BENCHMARK_NAMESPACE_POLICY "explicit_weak")
    set(BENCHMARK_PERFORMANCE_POLICY "diagnostic-only")
    set(benchmark_run_test benchmark.native_baseline)
    list(APPEND benchmark_policy_arguments --diagnostic-only)
  else()
    if(CMAKE_OSX_ARCHITECTURES)
      string(REPLACE ";" "," BENCHMARK_TARGET_ARCHITECTURE "${CMAKE_OSX_ARCHITECTURES}")
    else()
      set(BENCHMARK_TARGET_ARCHITECTURE "${CMAKE_SYSTEM_PROCESSOR}")
    endif()
    set(BENCHMARK_MODERN_FILE_ACCESS "platform_default")
    set(BENCHMARK_NAMESPACE_POLICY "directory_sync")
    set(BENCHMARK_PERFORMANCE_POLICY "severe-regression")
    set(benchmark_run_test benchmark.severe_regression)
  endif()
  set(benchmark_configuration "$<IF:$<BOOL:$<CONFIG>>,$<CONFIG>,unspecified>")
  set(benchmark_modes default)
  if(MODERN_LEVELDB_HAVE_WINDOWS_FILE_SYSTEM)
    list(APPEND benchmark_modes mapped)
  endif()
  foreach(benchmark_mode IN LISTS benchmark_modes)
    set(benchmark_target modern_leveldb_bench)
    set(benchmark_policy_target modern_leveldb_benchmark_policy)
    set(benchmark_policy_file benchmark_policy)
    set(benchmark_report_file results)
    set(benchmark_mode_test "${benchmark_run_test}")
    if(benchmark_mode STREQUAL "mapped")
      set(benchmark_target modern_leveldb_mapped_bench)
      set(benchmark_policy_target modern_leveldb_mapped_benchmark_policy)
      set(benchmark_policy_file benchmark_policy-mapped)
      set(benchmark_report_file results-mapped)
      set(benchmark_mode_test benchmark.native_mapped_baseline)
      set(BENCHMARK_MODERN_FILE_ACCESS "mapped_default")
    endif()
    add_executable("${benchmark_target}" db_bench.cc)
    if(benchmark_mode STREQUAL "mapped")
      target_compile_definitions("${benchmark_target}" PRIVATE MODERN_LEVELDB_BENCHMARK_MAPPED_DEFAULT=1)
    endif()
    target_include_directories("${benchmark_target}" PRIVATE "${PROJECT_SOURCE_DIR}/tests")
    target_link_libraries("${benchmark_target}" PRIVATE modern_leveldb::modern_leveldb leveldb)
    modern_leveldb_set_project_warnings("${benchmark_target}" PRIVATE)
    configure_file(benchmark_policy.json.in "${benchmark_policy_file}.configured.json" @ONLY)
    file(GENERATE
      OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/${benchmark_policy_file}-base-${benchmark_configuration}.json"
      INPUT "${CMAKE_CURRENT_BINARY_DIR}/${benchmark_policy_file}.configured.json"
    )
    add_custom_target("${benchmark_policy_target}"
      COMMAND "${CMAKE_COMMAND}"
        "-DREFERENCE_SOURCE=${MODERN_LEVELDB_REFERENCE_SOURCE_DIR}"
        "-DREFERENCE_OVERRIDE=${FETCHCONTENT_SOURCE_DIR_MODERN_LEVELDB_REFERENCE}"
        "-DPOLICY_BASE=${CMAKE_CURRENT_BINARY_DIR}/${benchmark_policy_file}-base-${benchmark_configuration}.json"
        "-DPOLICY_OUTPUT=${CMAKE_CURRENT_BINARY_DIR}/${benchmark_policy_file}-${benchmark_configuration}.json"
        -P "${PROJECT_SOURCE_DIR}/cmake/RefreshBenchmarkPolicy.cmake"
      VERBATIM
    )
    add_dependencies("${benchmark_policy_target}" leveldb)
    add_dependencies("${benchmark_target}" "${benchmark_policy_target}")
    add_test(
      NAME "${benchmark_mode_test}"
      COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/check_benchmark.py"
        "${CMAKE_CURRENT_BINARY_DIR}/${benchmark_report_file}-${benchmark_configuration}.json"
        --run "$<TARGET_FILE:${benchmark_target}>"
        --policy-sidecar "${CMAKE_CURRENT_BINARY_DIR}/${benchmark_policy_file}-${benchmark_configuration}.json"
        ${benchmark_policy_arguments}
    )
    set_tests_properties("${benchmark_mode_test}" PROPERTIES LABELS benchmark TIMEOUT 240)
  endforeach()

  add_test(
    NAME benchmark.checker
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/tools/benchmark_gate_test.py"
  )
  set_tests_properties(benchmark.checker PROPERTIES LABELS benchmark TIMEOUT 30)
  add_test(
    NAME benchmark.reference_provenance
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/tools/benchmark_policy_test.py"
      --cmake "${CMAKE_COMMAND}" --source "${PROJECT_SOURCE_DIR}"
  )
  set_tests_properties(benchmark.reference_provenance PROPERTIES LABELS benchmark TIMEOUT 120)
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
  set(
    PROFILE_EXE_LINKER_FLAGS
    "${CMAKE_EXE_LINKER_FLAGS} ${CMAKE_EXE_LINKER_FLAGS_${profile_config}}"
  )
  set(
    PROFILE_STATIC_LINKER_FLAGS
    "${CMAKE_STATIC_LINKER_FLAGS} ${CMAKE_STATIC_LINKER_FLAGS_${profile_config}}"
  )
  set(PROFILE_CMAKE_GENERATOR "${CMAKE_GENERATOR}")
  if(CMAKE_OSX_ARCHITECTURES)
    string(REPLACE ";" "," PROFILE_TARGET_ARCHITECTURE "${CMAKE_OSX_ARCHITECTURES}")
  else()
    set(PROFILE_TARGET_ARCHITECTURE "${CMAKE_SYSTEM_PROCESSOR}")
  endif()
  modern_leveldb_profile_markers(MODERN_LEVELDB_PROFILE_MARKERS)
  configure_file(profiling_build.h.in profiling_build.h @ONLY)

  add_executable(modern_leveldb_performance profiling_bench.cc)
  target_compile_definitions(
    modern_leveldb_performance PRIVATE MODERN_LEVELDB_READ_DIAGNOSTICS=0
  )
  target_include_directories(
    modern_leveldb_performance
    PRIVATE
      "${CMAKE_CURRENT_BINARY_DIR}"
      "${MODERN_LEVELDB_REFERENCE_SOURCE_DIR}"
  )
  target_link_libraries(
    modern_leveldb_performance PRIVATE modern_leveldb::modern_leveldb leveldb benchmark::benchmark
  )
  modern_leveldb_set_project_warnings(modern_leveldb_performance PRIVATE)

  add_executable(modern_leveldb_read_diagnostics profiling_bench.cc)
  target_include_directories(
    modern_leveldb_read_diagnostics
    PRIVATE
      "${CMAKE_CURRENT_BINARY_DIR}"
      "${PROJECT_SOURCE_DIR}/src"
      "${MODERN_LEVELDB_REFERENCE_SOURCE_DIR}"
  )
  target_link_libraries(
    modern_leveldb_read_diagnostics
    PRIVATE modern_leveldb_read_diagnostic leveldb benchmark::benchmark
  )
  modern_leveldb_set_project_warnings(modern_leveldb_read_diagnostics PRIVATE)

  add_test(
    NAME performance.contracts
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/tools/performance_test.py"
      --binary "$<TARGET_FILE:modern_leveldb_performance>"
      --reference-pread-control "${MODERN_LEVELDB_REFERENCE_PREAD_CONTROL}"
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
    NAME performance.write_parity_contracts
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/tools/write_parity_test.py"
  )
  add_test(
    NAME performance.smoke
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/tools/performance_smoke.py"
      --binary "$<TARGET_FILE:modern_leveldb_performance>"
      --output "${CMAKE_CURRENT_BINARY_DIR}/performance-smoke"
  )
  add_test(
    NAME performance.mutation_stream
    COMMAND "$<TARGET_FILE:modern_leveldb_performance>" --check-mutation-stream
  )
  add_test(
    NAME performance.mutations
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/tools/performance_smoke.py"
      --binary "$<TARGET_FILE:modern_leveldb_performance>"
      --output "${CMAKE_CURRENT_BINARY_DIR}/performance-mutations"
      --full-mutations
  )
  add_test(
    NAME performance.read_diagnostics
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/tools/read_diagnostics_smoke.py"
      --binary "$<TARGET_FILE:modern_leveldb_read_diagnostics>"
      --output "${CMAKE_CURRENT_BINARY_DIR}/performance-read-diagnostics"
  )
  set_tests_properties(
    performance.contracts performance.profile_parser performance.process_control
    performance.write_parity_contracts performance.mutation_stream
    PROPERTIES LABELS performance TIMEOUT 120
  )
  set_tests_properties(
    performance.smoke performance.mutations performance.read_diagnostics
    PROPERTIES LABELS performance TIMEOUT 600
  )
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
  add_test(
    NAME performance.reference_declaration
    COMMAND "${CMAKE_COMMAND}"
      "-DCONSUMER_SOURCE_DIR=${PROJECT_SOURCE_DIR}/tests/cmake/reference_declaration"
      "-DBINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}/reference-declaration"
      "-DGENERATOR=${CMAKE_GENERATOR}" "-DCOMPILER=${CMAKE_CXX_COMPILER}"
      "-DSOURCE_DIR=${PROJECT_SOURCE_DIR}"
      "-DEXPECTED_ERROR=predeclared LevelDB reference dependency"
      "-DSENTINEL=${PROJECT_SOURCE_DIR}/tests/cmake/reference_declaration/source/sentinel.txt"
      -P "${PROJECT_SOURCE_DIR}/tests/cmake/reject_reference.cmake"
  )
  add_test(
    NAME performance.reference_provider
    COMMAND "${CMAKE_COMMAND}"
      "-DCONSUMER_SOURCE_DIR=${PROJECT_SOURCE_DIR}/tests/cmake/reference_provider"
      "-DBINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}/reference-provider"
      "-DGENERATOR=${CMAKE_GENERATOR}" "-DCOMPILER=${CMAKE_CXX_COMPILER}"
      "-DSOURCE_DIR=${PROJECT_SOURCE_DIR}"
      "-DEXPECTED_ERROR=provider-populated"
      "-DSENTINEL=${PROJECT_SOURCE_DIR}/tests/cmake/reference_provider/source/sentinel.txt"
      -P "${PROJECT_SOURCE_DIR}/tests/cmake/reject_reference.cmake"
  )
  set_tests_properties(
    performance.capture_capabilities performance.dependency_declaration
    performance.reference_declaration performance.reference_provider
    PROPERTIES LABELS performance TIMEOUT 120
  )
endif()
