if(NOT DEFINED SOURCE_DIR OR NOT DEFINED BINARY_DIR)
  message(FATAL_ERROR "SOURCE_DIR and BINARY_DIR are required")
endif()

file(REMOVE_RECURSE "${BINARY_DIR}")
file(MAKE_DIRECTORY "${BINARY_DIR}/util")
set(original [=[
class Helper {
 private:
  // Set the maximum number of read-only files that will be mapped via mmap.
  // Must be called before creating an Env.
  static void SetReadOnlyMMapLimit(int limit);
};
]=])
set(exposed [=[
class Helper {
 private:
 public:
  // Set the maximum number of read-only files that will be mapped via mmap.
  // Must be called before creating an Env.
  static void SetReadOnlyMMapLimit(int limit);
};
]=])

foreach(platform IN ITEMS posix windows)
  set(header "${BINARY_DIR}/util/env_${platform}_test_helper.h")
  file(WRITE "${header}" "${original}")
  foreach(attempt RANGE 1 2)
    execute_process(
      COMMAND "${CMAKE_COMMAND}" "-DLEVELDB_SOURCE_DIR=${BINARY_DIR}"
        "-DLEVELDB_CONTROL_PLATFORM=${platform}"
        -P "${SOURCE_DIR}/cmake/ExposeLevelDbMmapLimit.cmake"
      RESULT_VARIABLE result
    )
    if(NOT result EQUAL 0)
      message(FATAL_ERROR "Could not expose the ${platform} reference control")
    endif()
  endforeach()
  file(READ "${header}" contents)
  if(NOT contents STREQUAL exposed)
    message(FATAL_ERROR "${platform} control patch changed more than declaration visibility")
  endif()
endforeach()

execute_process(
  COMMAND "${CMAKE_COMMAND}" "-DLEVELDB_SOURCE_DIR=${BINARY_DIR}"
    -DLEVELDB_CONTROL_PLATFORM=other
    -P "${SOURCE_DIR}/cmake/ExposeLevelDbMmapLimit.cmake"
  RESULT_VARIABLE invalid
  OUTPUT_QUIET ERROR_QUIET
)
if(invalid EQUAL 0)
  message(FATAL_ERROR "Invalid reference control platform was accepted")
endif()
