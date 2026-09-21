include(FetchContent)

set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)

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
  unit/base/result_test.cc
  unit/toolchain_test.cc
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
  PROPERTIES LABELS unit
)
