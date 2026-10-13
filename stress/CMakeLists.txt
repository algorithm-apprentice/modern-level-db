set(stress_build_header "${CMAKE_CURRENT_BINARY_DIR}/stress_build.h")
add_custom_target(
  modern_leveldb_stress_build_identity
  COMMAND
    "${CMAKE_COMMAND}"
      "-DSOURCE_DIR=${PROJECT_SOURCE_DIR}"
      "-DOUTPUT=${stress_build_header}"
      -P "${PROJECT_SOURCE_DIR}/cmake/RefreshStressIdentity.cmake"
  BYPRODUCTS "${stress_build_header}"
  VERBATIM
)

add_executable(
  modern_leveldb_stress
  stress_main.cc
  stress.cc
  stress_support.cc
  "${stress_build_header}"
)
add_dependencies(modern_leveldb_stress modern_leveldb_stress_build_identity)
target_include_directories(
  modern_leveldb_stress
  PRIVATE
    "${PROJECT_SOURCE_DIR}"
    "${PROJECT_SOURCE_DIR}/tests"
    "${CMAKE_CURRENT_BINARY_DIR}"
)
target_link_libraries(modern_leveldb_stress PRIVATE modern_leveldb::modern_leveldb)
modern_leveldb_set_project_warnings(modern_leveldb_stress PRIVATE)

add_executable(
  modern_leveldb_concurrent_stress
  concurrent_stress_main.cc
  concurrent_stress.cc
  linearizability.cc
  stress_support.cc
  "${stress_build_header}"
)
add_dependencies(modern_leveldb_concurrent_stress modern_leveldb_stress_build_identity)
target_include_directories(
  modern_leveldb_concurrent_stress
  PRIVATE
    "${PROJECT_SOURCE_DIR}"
    "${PROJECT_SOURCE_DIR}/tests"
    "${CMAKE_CURRENT_BINARY_DIR}"
)
target_link_libraries(modern_leveldb_concurrent_stress PRIVATE modern_leveldb::modern_leveldb)
modern_leveldb_set_project_warnings(modern_leveldb_concurrent_stress PRIVATE)
