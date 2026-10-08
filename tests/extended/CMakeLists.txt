function(modern_leveldb_extended_test name tier)
  add_executable("${name}" ${ARGN})
  target_include_directories(
    "${name}" PRIVATE "${PROJECT_SOURCE_DIR}/src" "${PROJECT_SOURCE_DIR}/tests"
  )
  target_link_libraries("${name}" PRIVATE modern_leveldb::modern_leveldb GTest::gtest_main)
  modern_leveldb_set_project_warnings("${name}" PRIVATE)
  gtest_discover_tests("${name}" PROPERTIES LABELS "${tier}" TIMEOUT 180)
endfunction()

modern_leveldb_extended_test(
  modern_leveldb_model_tests model
  ../model/database_model_test.cc
)
modern_leveldb_extended_test(
  modern_leveldb_compatibility_tests compatibility
  ../compatibility/database_differential_test.cc
  ../compatibility/golden_database_test.cc
)
target_compile_definitions(
  modern_leveldb_compatibility_tests PRIVATE
  MODERN_LEVELDB_GOLDEN_DATABASE="${PROJECT_SOURCE_DIR}/tests/compatibility/fixtures/leveldb-db.txt"
)
target_link_libraries(modern_leveldb_compatibility_tests PRIVATE leveldb)
modern_leveldb_extended_test(
  modern_leveldb_crash_tests crash
  ../crash/power_loss_test.cc
  ../support/crash_file_system.cc
  ../support/memory_file_system.cc
)
if(MODERN_LEVELDB_HAVE_WINDOWS_FILE_SYSTEM)
  add_executable(modern_leveldb_windows_crash_child ../support/windows_crash_child.cc)
  target_include_directories(
    modern_leveldb_windows_crash_child PRIVATE "${PROJECT_SOURCE_DIR}/src"
  )
  target_link_libraries(modern_leveldb_windows_crash_child PRIVATE modern_leveldb::modern_leveldb)
  modern_leveldb_set_project_warnings(modern_leveldb_windows_crash_child PRIVATE)
  target_sources(
    modern_leveldb_crash_tests
    PRIVATE ../crash/process_crash_windows_test.cc ../support/windows_crash_process.cc
  )
  target_compile_definitions(
    modern_leveldb_crash_tests
    PRIVATE MODERN_LEVELDB_WINDOWS_CRASH_CHILD=L"$<TARGET_FILE:modern_leveldb_windows_crash_child>"
  )
  target_compile_options(
    modern_leveldb_crash_tests PRIVATE "$<$<CXX_COMPILER_ID:MSVC>:/utf-8>"
  )
  add_dependencies(modern_leveldb_crash_tests modern_leveldb_windows_crash_child)
else()
  target_sources(modern_leveldb_crash_tests PRIVATE ../crash/process_crash_test.cc)
endif()

add_custom_target(
  modern_leveldb_extended_tests
  DEPENDS modern_leveldb_model_tests modern_leveldb_compatibility_tests modern_leveldb_crash_tests
)
