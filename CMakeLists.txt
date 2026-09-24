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

include("${CMAKE_CURRENT_LIST_DIR}/cmake/CompilerWarnings.cmake")
find_package(Threads REQUIRED)

add_library(
  modern_leveldb
  STATIC
    src/base/coding.cc
    src/base/comparator.cc
    src/base/crc32c.cc
    src/base/hash.cc
    src/engine/build_table.cc
    src/engine/table_cache.cc
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
target_link_libraries(modern_leveldb PUBLIC Threads::Threads)
modern_leveldb_set_project_warnings(modern_leveldb PRIVATE)

if(MODERN_LEVELDB_BUILD_TESTS)
  include(CTest)
  if(BUILD_TESTING)
    add_subdirectory(tests)
  endif()
endif()
