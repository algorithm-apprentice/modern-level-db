function(modern_leveldb_profile_markers output)
  set(supported 0)
  if(APPLE AND CMAKE_CXX_COMPILER_ID STREQUAL "AppleClang")
    set(supported 1)
  endif()
  set("${output}" "${supported}" PARENT_SCOPE)
endfunction()

function(modern_leveldb_add_google_benchmark)
  include(FetchContent)
  if(TARGET benchmark OR TARGET benchmark::benchmark)
    message(FATAL_ERROR "Performance tests require their pinned Google Benchmark target")
  endif()
  # GetProperties exposes population, not prior declarations. Guard the saved-details
  # registry as well; the consumer fixture checks this CMake integration boundary.
  get_property(
    declared GLOBAL PROPERTY _FetchContent_modern_leveldb_benchmark_savedDetails DEFINED
  )
  if(declared)
    message(FATAL_ERROR "Performance tests reject a predeclared Google Benchmark dependency")
  endif()
  foreach(
    option
    IN ITEMS
      BENCHMARK_ENABLE_TESTING
      BENCHMARK_ENABLE_GTEST_TESTS
      BENCHMARK_ENABLE_ASSEMBLY_TESTS
      BENCHMARK_ENABLE_DOXYGEN
      BENCHMARK_ENABLE_INSTALL
      BENCHMARK_INSTALL_DOCS
      BENCHMARK_INSTALL_TOOLS
      BENCHMARK_DOWNLOAD_DEPENDENCIES
      BENCHMARK_ENABLE_LIBPFM
  )
    if(NOT DEFINED "${option}")
      set("${option}" OFF)
    endif()
  endforeach()
  if(NOT DEFINED BUILD_SHARED_LIBS)
    set(BUILD_SHARED_LIBS OFF)
  endif()
  FetchContent_Declare(
    modern_leveldb_benchmark
    URL "https://github.com/google/benchmark/archive/192ef10025eb2c4cdd392bc502f0c852196baa48.tar.gz"
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    SYSTEM
  )
  FetchContent_MakeAvailable(modern_leveldb_benchmark)
  set_property(
    DIRECTORY "${modern_leveldb_benchmark_SOURCE_DIR}"
    PROPERTY EXCLUDE_FROM_ALL TRUE
  )
endfunction()
