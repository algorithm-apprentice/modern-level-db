execute_process(
  COMMAND "${CMAKE_COMMAND}" --fresh
    -S "${CMAKE_CURRENT_LIST_DIR}/consumer" -B "${BINARY_DIR}" -G "${GENERATOR}"
    "-DCMAKE_CXX_COMPILER=${COMPILER}"
    "-DMODERN_LEVELDB_SOURCE_DIR=${SOURCE_DIR}"
    -DTEST_SCENARIO=benchmark_reference_collision
    "-DFETCHCONTENT_SOURCE_DIR_MODERN_LEVELDB_SNAPPY=${SNAPPY_SOURCE}"
    "-DFETCHCONTENT_SOURCE_DIR_MODERN_LEVELDB_ZSTD=${ZSTD_SOURCE}"
  RESULT_VARIABLE result
  OUTPUT_VARIABLE output
  ERROR_VARIABLE error
)
if(result EQUAL 0)
  message(FATAL_ERROR "The benchmark accepted an arbitrary parent LevelDB target")
endif()
if(NOT "${output}${error}" MATCHES "require their pinned LevelDB reference target")
  message(FATAL_ERROR "Configuration failed for the wrong reason:\n${output}\n${error}")
endif()
