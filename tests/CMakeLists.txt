include(FetchContent)

if(NOT DEFINED BUILD_GMOCK)
  set(BUILD_GMOCK OFF)
endif()
if(NOT DEFINED INSTALL_GTEST)
  set(INSTALL_GTEST OFF)
endif()
if(NOT DEFINED gtest_force_shared_crt)
  set(gtest_force_shared_crt ON)
endif()

FetchContent_Declare(
  googletest
  GIT_REPOSITORY https://github.com/google/googletest.git
  GIT_TAG v1.17.0
  GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(googletest)

add_executable(
  modern_leveldb_unit_tests
  unit/base/bytes_test.cc
  unit/base/coding_test.cc
  unit/base/comparator_test.cc
  unit/base/crc32c_test.cc
  unit/base/hash_test.cc
  unit/base/result_test.cc
  unit/cache/sharded_lru_cache_test.cc
  unit/engine/build_table_test.cc
  unit/engine/lookup_test.cc
  unit/engine/recovery_test.cc
  unit/engine/table_cache_test.cc
  unit/format/internal_key_test.cc
  unit/format/wal_format_test.cc
  unit/format/write_batch_test.cc
  unit/memory/arena_test.cc
  unit/memory/memtable_test.cc
  unit/memory/skiplist_test.cc
  unit/metadata/filenames_test.cc
  unit/metadata/version_edit_test.cc
  unit/metadata/version_set_test.cc
  unit/metadata/version_test.cc
  unit/platform/clock_test.cc
  unit/platform/file_system_test.cc
  unit/platform/serial_executor_test.cc
  unit/table/block_format_test.cc
  unit/table/block_test.cc
  unit/table/bloom_filter_test.cc
  unit/table/filter_block_test.cc
  unit/table/table_builder_test.cc
  unit/table/table_test.cc
  unit/toolchain_test.cc
  unit/wal/wal_io_test.cc
  support/memory_file_system.cc
)
if(APPLE OR CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_sources(
    modern_leveldb_unit_tests
    PRIVATE
      unit/platform/posix_file_system_test.cc
      unit/wal/wal_io_posix_test.cc
  )
endif()
target_include_directories(
  modern_leveldb_unit_tests
  PRIVATE "${PROJECT_SOURCE_DIR}/src" "${CMAKE_CURRENT_SOURCE_DIR}"
)
target_link_libraries(
  modern_leveldb_unit_tests
  PRIVATE
    modern_leveldb::modern_leveldb
    GTest::gtest_main
)
modern_leveldb_set_project_warnings(modern_leveldb_unit_tests PRIVATE)

include(GoogleTest)
gtest_discover_tests(
  modern_leveldb_unit_tests
  PROPERTIES
    LABELS unit
    TIMEOUT 10
)

foreach(scenario IN ITEMS module_shadow default_tests tests_disabled gtest_options)
  add_test(
    NAME "cmake.${scenario}"
    COMMAND "${CMAKE_COMMAND}"
      --fresh
      -S "${CMAKE_CURRENT_SOURCE_DIR}/cmake/consumer"
      -B "${CMAKE_CURRENT_BINARY_DIR}/cmake/${scenario}"
      -G "${CMAKE_GENERATOR}"
      "-DCMAKE_CXX_COMPILER=${CMAKE_CXX_COMPILER}"
      "-DMODERN_LEVELDB_SOURCE_DIR=${PROJECT_SOURCE_DIR}"
      "-DTEST_SCENARIO=${scenario}"
      "-DFETCHCONTENT_SOURCE_DIR_GOOGLETEST=${googletest_SOURCE_DIR}"
  )
  set_tests_properties("cmake.${scenario}" PROPERTIES LABELS cmake TIMEOUT 120)
endforeach()
