add_executable(modern_leveldb_bench db_bench.cc)
target_include_directories(modern_leveldb_bench PRIVATE "${PROJECT_SOURCE_DIR}/tests")
target_link_libraries(modern_leveldb_bench PRIVATE modern_leveldb::modern_leveldb leveldb)
modern_leveldb_set_project_warnings(modern_leveldb_bench PRIVATE)

find_package(Python3 3.9 REQUIRED COMPONENTS Interpreter)
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
