if(NOT DEFINED SOURCE_DIR OR NOT DEFINED OUTPUT)
  message(FATAL_ERROR "RefreshStressIdentity requires SOURCE_DIR and OUTPUT")
endif()

execute_process(
  COMMAND git -C "${SOURCE_DIR}" rev-parse HEAD
  RESULT_VARIABLE revision_status
  OUTPUT_VARIABLE source_revision
  OUTPUT_STRIP_TRAILING_WHITESPACE
  ERROR_QUIET
)
execute_process(
  COMMAND git -C "${SOURCE_DIR}" status --porcelain --untracked-files=normal
  RESULT_VARIABLE dirty_status
  OUTPUT_VARIABLE source_changes
  ERROR_QUIET
)
set(source_dirty 1)
if(revision_status EQUAL 0 AND dirty_status EQUAL 0)
  if(source_changes STREQUAL "")
    set(source_dirty 0)
  endif()
else()
  set(source_revision "unavailable")
endif()

set(
  contents
  "#ifndef MODERN_LEVELDB_STRESS_STRESS_BUILD_H_\n"
  "#define MODERN_LEVELDB_STRESS_STRESS_BUILD_H_\n\n"
  "#define MODERN_LEVELDB_STRESS_SOURCE_REVISION \"${source_revision}\"\n"
  "#define MODERN_LEVELDB_STRESS_SOURCE_DIRTY ${source_dirty}\n\n"
  "#endif  // MODERN_LEVELDB_STRESS_STRESS_BUILD_H_\n"
)
string(JOIN "" contents ${contents})

set(previous "")
if(EXISTS "${OUTPUT}")
  file(READ "${OUTPUT}" previous)
endif()
if(NOT previous STREQUAL contents)
  file(WRITE "${OUTPUT}" "${contents}")
endif()
