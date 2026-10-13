execute_process(
  COMMAND git -C "${PROJECT_SOURCE_DIR}" rev-parse HEAD
  RESULT_VARIABLE revision_status
  OUTPUT_VARIABLE STRESS_SOURCE_REVISION
  OUTPUT_STRIP_TRAILING_WHITESPACE
  ERROR_QUIET
)
execute_process(
  COMMAND git -C "${PROJECT_SOURCE_DIR}" status --porcelain --untracked-files=normal
  RESULT_VARIABLE dirty_status
  OUTPUT_VARIABLE source_changes
  ERROR_QUIET
)
set(STRESS_SOURCE_DIRTY 1)
if(revision_status EQUAL 0 AND dirty_status EQUAL 0)
  if(source_changes STREQUAL "")
    set(STRESS_SOURCE_DIRTY 0)
  endif()
else()
  set(STRESS_SOURCE_REVISION "unavailable")
  message(STATUS "Stress source revision is unavailable in this checkout")
endif()
configure_file(stress_build.h.in stress_build.h @ONLY)

add_executable(modern_leveldb_stress stress_main.cc stress.cc)
target_include_directories(
  modern_leveldb_stress
  PRIVATE
    "${PROJECT_SOURCE_DIR}"
    "${PROJECT_SOURCE_DIR}/tests"
    "${CMAKE_CURRENT_BINARY_DIR}"
)
target_link_libraries(modern_leveldb_stress PRIVATE modern_leveldb::modern_leveldb)
modern_leveldb_set_project_warnings(modern_leveldb_stress PRIVATE)
