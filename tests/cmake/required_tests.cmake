set(fixture_source "${BINARY_DIR}/source")
file(MAKE_DIRECTORY "${fixture_source}")
file(COPY_FILE "${SOURCE_DIR}/CMakePresets.json" "${fixture_source}/CMakePresets.json")
file(WRITE "${fixture_source}/CMakeLists.txt" [=[
cmake_minimum_required(VERSION 3.25)
project(RequiredTests NONE)
enable_testing()
add_test(NAME fixture.pass COMMAND "${CMAKE_COMMAND}" -E true)
set_tests_properties(
  fixture.pass PROPERTIES LABELS "unit;model;compatibility;crash;benchmark;performance;fuzz"
)
]=])

file(READ "${SOURCE_DIR}/CMakePresets.json" presets)
string(JSON configure_count LENGTH "${presets}" configurePresets)
math(EXPR last_configure "${configure_count} - 1")
foreach(index RANGE 0 "${last_configure}")
  string(JSON binary_dir GET "${presets}" configurePresets "${index}" binaryDir)
  string(REPLACE "\${sourceDir}" "${fixture_source}" binary_dir "${binary_dir}")
  execute_process(
    COMMAND "${CMAKE_COMMAND}" --fresh -S "${fixture_source}" -B "${binary_dir}" -G "${GENERATOR}"
    RESULT_VARIABLE configured
    OUTPUT_VARIABLE configure_output
    ERROR_VARIABLE configure_error
  )
  if(NOT configured EQUAL 0)
    message(FATAL_ERROR "Fixture configuration failed:\n${configure_output}\n${configure_error}")
  endif()
endforeach()

string(JSON test_count LENGTH "${presets}" testPresets)
math(EXPR last_test "${test_count} - 1")
set(checked 0)
foreach(index RANGE 0 "${last_test}")
  string(JSON hidden ERROR_VARIABLE hidden_error GET "${presets}" testPresets "${index}" hidden)
  if(hidden STREQUAL "ON")
    continue()
  endif()
  string(JSON name GET "${presets}" testPresets "${index}" name)
  set(pass_report "${BINARY_DIR}/${name}.xml")
  file(REMOVE "${pass_report}")
  execute_process(
    COMMAND "${CTEST_COMMAND}" --preset "${name}" --output-junit "${pass_report}"
    WORKING_DIRECTORY "${fixture_source}"
    RESULT_VARIABLE passed
    OUTPUT_VARIABLE pass_output
    ERROR_VARIABLE pass_error
  )
  if(NOT passed EQUAL 0 OR NOT EXISTS "${pass_report}")
    message(FATAL_ERROR "Preset ${name} did not run its test:\n${pass_output}\n${pass_error}")
  endif()
  file(READ "${pass_report}" report)
  string(REGEX MATCH "<testsuite[ \t\r\n][^>]*>" suite "${report}")
  if(NOT suite MATCHES "[ \t\r\n]tests=\"1\"" OR
     NOT suite MATCHES "[ \t\r\n]failures=\"0\"" OR
     NOT suite MATCHES "[ \t\r\n]skipped=\"0\"")
    message(FATAL_ERROR "Preset ${name} did not execute exactly one passing test:\n${report}")
  endif()

  execute_process(
    COMMAND "${CTEST_COMMAND}" --preset "${name}" -R "^no-matching-test$"
    WORKING_DIRECTORY "${fixture_source}"
    RESULT_VARIABLE rejected
    OUTPUT_VARIABLE empty_output
    ERROR_VARIABLE empty_error
  )
  if(rejected EQUAL 0 OR NOT "${empty_output}\n${empty_error}" MATCHES "No tests were found")
    message(FATAL_ERROR
      "Preset ${name} did not reject an empty selection:\n${empty_output}\n${empty_error}"
    )
  endif()
  math(EXPR checked "${checked} + 1")
endforeach()

if(checked EQUAL 0)
  message(FATAL_ERROR "No visible test presets were checked")
endif()
message(STATUS "Verified nonempty and empty selections for ${checked} test presets")
