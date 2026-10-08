add_executable(modern_leveldb_tool modern_leveldb_tool.cc)
target_include_directories(modern_leveldb_tool PRIVATE "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(modern_leveldb_tool PRIVATE modern_leveldb::modern_leveldb)
modern_leveldb_set_project_warnings(modern_leveldb_tool PRIVATE)

if(MODERN_LEVELDB_BUILD_TESTS AND BUILD_TESTING)
  find_package(Python3 COMPONENTS Interpreter REQUIRED)
  add_executable(modern_leveldb_diagnostic_fixture ../tests/support/diagnostic_fixture.cc)
  target_include_directories(modern_leveldb_diagnostic_fixture PRIVATE "${PROJECT_SOURCE_DIR}/src")
  target_link_libraries(modern_leveldb_diagnostic_fixture PRIVATE modern_leveldb::modern_leveldb)
  modern_leveldb_set_project_warnings(modern_leveldb_diagnostic_fixture PRIVATE)
  add_test(
    NAME tool.native_contracts
    COMMAND "${Python3_EXECUTABLE}" "${PROJECT_SOURCE_DIR}/tests/tools/diagnostic_tool_test.py"
      --tool "$<TARGET_FILE:modern_leveldb_tool>"
      --fixture "$<TARGET_FILE:modern_leveldb_diagnostic_fixture>"
  )
  set_tests_properties(tool.native_contracts PROPERTIES LABELS unit TIMEOUT 120)
  add_test(NAME tool.help COMMAND modern_leveldb_tool --help)
  set_tests_properties(tool.help PROPERTIES LABELS unit TIMEOUT 10)
  add_test(NAME tool.usage COMMAND modern_leveldb_tool)
  set_tests_properties(tool.usage PROPERTIES LABELS unit TIMEOUT 10 WILL_FAIL TRUE)
endif()
