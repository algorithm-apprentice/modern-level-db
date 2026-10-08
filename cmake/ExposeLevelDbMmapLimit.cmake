if(NOT DEFINED LEVELDB_SOURCE_DIR)
  message(FATAL_ERROR "LEVELDB_SOURCE_DIR is required")
endif()

if(NOT DEFINED LEVELDB_CONTROL_PLATFORM)
  set(LEVELDB_CONTROL_PLATFORM posix)
endif()
if(NOT LEVELDB_CONTROL_PLATFORM STREQUAL "posix" AND
   NOT LEVELDB_CONTROL_PLATFORM STREQUAL "windows")
  message(FATAL_ERROR "Unsupported LevelDB control platform")
endif()
set(header "${LEVELDB_SOURCE_DIR}/util/env_${LEVELDB_CONTROL_PLATFORM}_test_helper.h")
if(NOT EXISTS "${header}")
  message(FATAL_ERROR "Pinned LevelDB test helper is missing: ${header}")
endif()

file(READ "${header}" contents)
set(original [=[
  // Set the maximum number of read-only files that will be mapped via mmap.
  // Must be called before creating an Env.
  static void SetReadOnlyMMapLimit(int limit);
]=])
set(exposed [=[
 public:
  // Set the maximum number of read-only files that will be mapped via mmap.
  // Must be called before creating an Env.
  static void SetReadOnlyMMapLimit(int limit);
]=])

string(FIND "${contents}" "${exposed}" exposed_at)
if(exposed_at GREATER_EQUAL 0)
  return()
endif()

string(FIND "${contents}" "${original}" original_at)
if(original_at LESS 0)
  message(FATAL_ERROR "Pinned LevelDB mmap-limit declaration changed")
endif()

string(REPLACE "${original}" "${exposed}" patched "${contents}")
file(WRITE "${header}" "${patched}")
