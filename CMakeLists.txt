cmake_minimum_required(VERSION 3.25)

project(
  modern_leveldb
  VERSION 0.1.0
  DESCRIPTION "A ground-up C++23 reimplementation of LevelDB"
  LANGUAGES CXX
)

option(
  MODERN_LEVELDB_BUILD_TESTS
  "Build Modern LevelDB tests"
  ${PROJECT_IS_TOP_LEVEL}
)
option(
  MODERN_LEVELDB_WARNINGS_AS_ERRORS
  "Treat Modern LevelDB compiler warnings as errors"
  ON
)
option(MODERN_LEVELDB_BUILD_EXTENDED_TESTS "Build model, compatibility, and crash tests" OFF)
option(MODERN_LEVELDB_BUILD_FUZZERS "Build coverage-guided libFuzzer targets" OFF)
option(MODERN_LEVELDB_BUILD_BENCHMARKS "Build the comparative database benchmark" OFF)
option(MODERN_LEVELDB_BUILD_PERFORMANCE_TESTS "Build the selected-workload profiling harness" OFF)

include("${CMAKE_CURRENT_LIST_DIR}/cmake/CompilerWarnings.cmake")
include("${CMAKE_CURRENT_LIST_DIR}/cmake/CompressionDependencies.cmake")
find_package(Threads REQUIRED)
modern_leveldb_compression_dependencies(
  MODERN_LEVELDB_SNAPPY_TARGET
  MODERN_LEVELDB_ZSTD_TARGET
)

add_library(
  modern_leveldb
  STATIC
    src/api/database.cc
    src/api/iterator.cc
    src/api/snapshot.cc
    src/api/write_batch.cc
    src/base/coding.cc
    src/base/comparator.cc
    src/base/crc32c.cc
    src/base/hash.cc
    src/engine/build_table.cc
    src/engine/compaction.cc
    src/engine/compaction_picker.cc
    src/engine/database.cc
    src/engine/db_iterator.cc
    src/engine/flush.cc
    src/engine/iterators.cc
    src/engine/lookup.cc
    src/engine/recovery.cc
    src/engine/seek_statistics.cc
    src/engine/table_cache.cc
    src/engine/write_path.cc
    src/format/internal_key.cc
    src/format/wal_format.cc
    src/format/write_batch.cc
    src/memory/arena.cc
    src/memory/memtable.cc
    src/metadata/filenames.cc
    src/metadata/version.cc
    src/metadata/version_edit.cc
    src/metadata/version_set.cc
    src/platform/clock.cc
    src/platform/serial_executor.cc
    src/table/block.cc
    src/table/block_builder.cc
    src/table/block_format.cc
    src/table/bloom_filter.cc
    src/table/compression.cc
    src/table/filter_block.cc
    src/table/table.cc
    src/table/table_builder.cc
    src/wal/wal_io.cc
)
if(APPLE OR CMAKE_SYSTEM_NAME STREQUAL "Linux")
  target_sources(
    modern_leveldb
    PRIVATE
      src/platform/posix_file_system.cc
  )
  target_compile_definitions(modern_leveldb PRIVATE MODERN_LEVELDB_HAVE_POSIX_FILE_SYSTEM)
endif()
add_library(modern_leveldb::modern_leveldb ALIAS modern_leveldb)

target_compile_features(modern_leveldb PUBLIC cxx_std_23)
target_include_directories(
  modern_leveldb
  PUBLIC
    "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include>"
    "$<INSTALL_INTERFACE:include>"
  PRIVATE
    "${PROJECT_SOURCE_DIR}/src"
)
target_link_libraries(
  modern_leveldb
  PUBLIC Threads::Threads
  PRIVATE
    "${MODERN_LEVELDB_SNAPPY_TARGET}"
    "${MODERN_LEVELDB_ZSTD_TARGET}"
)
modern_leveldb_set_project_warnings(modern_leveldb PRIVATE)

if(MODERN_LEVELDB_BUILD_TESTS)
  include(CTest)
endif()

if(MODERN_LEVELDB_BUILD_EXTENDED_TESTS)
  if(NOT MODERN_LEVELDB_BUILD_TESTS OR NOT BUILD_TESTING)
    message(FATAL_ERROR "Extended tests require MODERN_LEVELDB_BUILD_TESTS and BUILD_TESTING")
  endif()
endif()

if(MODERN_LEVELDB_BUILD_EXTENDED_TESTS OR MODERN_LEVELDB_BUILD_BENCHMARKS OR
   MODERN_LEVELDB_BUILD_PERFORMANCE_TESTS)
  if(NOT APPLE AND NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "The database harness and benchmarks require the POSIX backend")
  endif()
  if(MODERN_LEVELDB_BUILD_PERFORMANCE_TESTS)
    include("${CMAKE_CURRENT_LIST_DIR}/cmake/GoogleBenchmark.cmake")
    modern_leveldb_add_google_benchmark()
  endif()
  include("${CMAKE_CURRENT_LIST_DIR}/cmake/LevelDbReference.cmake")
  modern_leveldb_add_reference()
endif()

if(MODERN_LEVELDB_BUILD_TESTS AND BUILD_TESTING)
  add_subdirectory(tests)
endif()

if(MODERN_LEVELDB_BUILD_FUZZERS)
  enable_testing()
  add_subdirectory(fuzz)
endif()

if(MODERN_LEVELDB_BUILD_BENCHMARKS OR MODERN_LEVELDB_BUILD_PERFORMANCE_TESTS)
  enable_testing()
  add_subdirectory(benchmarks)
endif()
