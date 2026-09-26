include(FetchContent)

function(modern_leveldb_add_reference)
  if(TARGET leveldb)
    message(FATAL_ERROR "Harness and benchmarks require their pinned LevelDB reference target")
  endif()
  set(BUILD_SHARED_LIBS OFF)
  set(LEVELDB_BUILD_TESTS OFF)
  set(LEVELDB_BUILD_BENCHMARKS OFF)
  set(LEVELDB_INSTALL OFF)
  set(HAVE_SNAPPY 1)
  set(HAVE_ZSTD 1)
  set(HAVE_CRC32C 0)
  set(HAVE_TCMALLOC 0)
  FetchContent_Declare(
    modern_leveldb_reference
    URL "https://github.com/google/leveldb/archive/7ee830d02b623e8ffe0b95d59a74db1e58da04c5.tar.gz"
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SYSTEM
  )
  FetchContent_MakeAvailable(modern_leveldb_reference)
  set_property(
    DIRECTORY "${modern_leveldb_reference_SOURCE_DIR}"
    PROPERTY EXCLUDE_FROM_ALL TRUE
  )
  # Upstream probes system libraries and links bare names; use the verified targets.
  set_property(
    TARGET leveldb PROPERTY LINK_LIBRARIES
    "${MODERN_LEVELDB_SNAPPY_TARGET};${MODERN_LEVELDB_ZSTD_TARGET};Threads::Threads"
  )
  set_property(
    TARGET leveldb PROPERTY INTERFACE_LINK_LIBRARIES
    "${MODERN_LEVELDB_SNAPPY_TARGET};${MODERN_LEVELDB_ZSTD_TARGET};Threads::Threads"
  )
  # Keep reference interfaces compatible with RTTI-enabled tests and UBSan vptr checks.
  target_compile_options(
    leveldb PRIVATE "$<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-frtti>"
  )
endfunction()
