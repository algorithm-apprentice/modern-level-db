if(NOT DEFINED TEST_SCENARIO)
  set(TEST_SCENARIO benchmark_reference_collision)
endif()
if(NOT DEFINED EXPECTED_ERROR)
  set(EXPECTED_ERROR "require their pinned LevelDB reference target")
endif()
if(NOT DEFINED CONSUMER_SOURCE_DIR)
  set(CONSUMER_SOURCE_DIR "${CMAKE_CURRENT_LIST_DIR}/consumer")
endif()

execute_process(
  COMMAND "${CMAKE_COMMAND}" --fresh
    -S "${CONSUMER_SOURCE_DIR}" -B "${BINARY_DIR}" -G "${GENERATOR}"
    "-DCMAKE_CXX_COMPILER=${COMPILER}"
    "-DMODERN_LEVELDB_SOURCE_DIR=${SOURCE_DIR}"
    "-DTEST_SCENARIO=${TEST_SCENARIO}"
    "-DFETCHCONTENT_SOURCE_DIR_MODERN_LEVELDB_SNAPPY=${SNAPPY_SOURCE}"
    "-DFETCHCONTENT_SOURCE_DIR_MODERN_LEVELDB_ZSTD=${ZSTD_SOURCE}"
    "-DFETCHCONTENT_SOURCE_DIR_MODERN_LEVELDB_CRC32C=${CRC32C_SOURCE}"
  RESULT_VARIABLE result
  OUTPUT_VARIABLE output
  ERROR_VARIABLE error
)
if(result EQUAL 0)
  message(FATAL_ERROR "The consumer accepted a conflicting dependency")
endif()
if(NOT "${output}${error}" MATCHES "${EXPECTED_ERROR}")
  message(FATAL_ERROR "Configuration failed for the wrong reason:\n${output}\n${error}")
endif()
