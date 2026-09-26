if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  message(FATAL_ERROR "Fuzzing requires Clang with the libFuzzer runtime")
endif()
include(CheckCXXSourceCompiles)
set(CMAKE_REQUIRED_FLAGS "-fsanitize=fuzzer")
check_cxx_source_compiles(
  "#include <cstddef>
   extern \"C\" int LLVMFuzzerTestOneInput(const unsigned char*, std::size_t) { return 0; }"
  MODERN_LEVELDB_HAVE_LIBFUZZER
)
if(NOT MODERN_LEVELDB_HAVE_LIBFUZZER)
  message(FATAL_ERROR "libFuzzer is missing; select a full LLVM toolchain (macOS: brew install llvm)")
endif()
find_package(Python3 3.9 REQUIRED COMPONENTS Interpreter)

target_compile_options(
  modern_leveldb PRIVATE -fsanitize=fuzzer-no-link,address,undefined -fno-omit-frame-pointer
)
target_link_options(modern_leveldb INTERFACE -fsanitize=address,undefined)

foreach(kind IN ITEMS formats database)
  if(kind STREQUAL "formats")
    set(source format_fuzzer.cc)
  else()
    set(source database_fuzzer.cc "${PROJECT_SOURCE_DIR}/tests/support/memory_file_system.cc")
  endif()
  set(target "modern_leveldb_${kind}_fuzzer")
  add_executable("${target}" ${source})
  target_include_directories(
    "${target}" PRIVATE "${PROJECT_SOURCE_DIR}/src" "${PROJECT_SOURCE_DIR}/tests"
  )
  target_link_libraries(
    "${target}" PRIVATE modern_leveldb::modern_leveldb
      "${MODERN_LEVELDB_SNAPPY_TARGET}" "${MODERN_LEVELDB_ZSTD_TARGET}"
  )
  target_compile_options(
    "${target}" PRIVATE -fsanitize=fuzzer-no-link,address,undefined -fno-omit-frame-pointer
  )
  target_link_options("${target}" PRIVATE -fsanitize=fuzzer,address,undefined)
  modern_leveldb_set_project_warnings("${target}" PRIVATE)
  add_test(
    NAME "fuzz.${kind}"
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tools/run_fuzz.py"
      --binary "$<TARGET_FILE:${target}>" --kind "${kind}"
      --output "${CMAKE_CURRENT_BINARY_DIR}/${kind}-smoke"
  )
  set_tests_properties("fuzz.${kind}" PROPERTIES LABELS fuzz TIMEOUT 300)
endforeach()
