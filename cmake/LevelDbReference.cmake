include(FetchContent)

function(modern_leveldb_add_reference)
  if(TARGET leveldb)
    message(FATAL_ERROR "Harness and benchmarks require their pinned LevelDB reference target")
  endif()
  get_property(
    declared GLOBAL PROPERTY _FetchContent_modern_leveldb_reference_savedDetails DEFINED
  )
  if(declared)
    message(FATAL_ERROR "Harness and benchmarks reject a predeclared LevelDB reference dependency")
  endif()
  set(BUILD_SHARED_LIBS OFF)
  set(LEVELDB_BUILD_TESTS OFF)
  set(LEVELDB_BUILD_BENCHMARKS OFF)
  set(LEVELDB_INSTALL OFF)
  set(HAVE_SNAPPY 1)
  set(HAVE_ZSTD 1)
  set(HAVE_CRC32C 0)
  set(HAVE_TCMALLOC 0)
  FetchContent_GetProperties(modern_leveldb_reference)
  if(modern_leveldb_reference_POPULATED)
    message(FATAL_ERROR "Harness and benchmarks reject a provider-populated LevelDB reference dependency")
  endif()
  # Keep the patchable reference private to this build even when a parent
  # redirects FetchContent's shared base directory.
  set(reference_fetch_root "${CMAKE_BINARY_DIR}/_deps")
  set(
    modern_leveldb_reference_BINARY_DIR
    "${reference_fetch_root}/modern_leveldb_reference-build"
  )
  if(FETCHCONTENT_SOURCE_DIR_MODERN_LEVELDB_REFERENCE)
    get_filename_component(
      modern_leveldb_reference_SOURCE_DIR
      "${FETCHCONTENT_SOURCE_DIR_MODERN_LEVELDB_REFERENCE}"
      REALPATH
      BASE_DIR "${CMAKE_BINARY_DIR}"
    )
  else()
    # The self-contained population form bypasses dependency providers, so
    # the source patched below is always the authenticated build-owned archive.
    FetchContent_Populate(
      modern_leveldb_reference
      URL "https://github.com/google/leveldb/archive/7ee830d02b623e8ffe0b95d59a74db1e58da04c5.tar.gz"
      URL_HASH SHA256=ae7117a5180fe263d10d77ec03b8109aaf93af8541615ff1361ef120c0506e9f
      DOWNLOAD_EXTRACT_TIMESTAMP TRUE
      SOURCE_DIR "${reference_fetch_root}/modern_leveldb_reference-src"
      BINARY_DIR "${modern_leveldb_reference_BINARY_DIR}"
      SUBBUILD_DIR "${reference_fetch_root}/modern_leveldb_reference-subbuild"
    )
  endif()
  set(reference_pread_control FALSE)
  set(reference_windows_control FALSE)
  set(reference_control_patch
      "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/ExposeLevelDbMmapLimit.cmake")
  file(SHA256 "${reference_control_patch}" reference_control_patch_sha256)
  if(MODERN_LEVELDB_BUILD_PERFORMANCE_TESTS AND
     MODERN_LEVELDB_HAVE_WINDOWS_FILE_SYSTEM AND
     NOT FETCHCONTENT_SOURCE_DIR_MODERN_LEVELDB_REFERENCE)
    execute_process(
      COMMAND "${CMAKE_COMMAND}"
        "-DLEVELDB_SOURCE_DIR=${modern_leveldb_reference_SOURCE_DIR}"
        -DLEVELDB_CONTROL_PLATFORM=windows
        -P "${reference_control_patch}"
      RESULT_VARIABLE patch_status
    )
    if(NOT patch_status EQUAL 0)
      message(FATAL_ERROR "Failed to expose the pinned native LevelDB mmap-limit control")
    endif()
    set(reference_windows_control TRUE)
  endif()
  if(MODERN_LEVELDB_BUILD_PERFORMANCE_TESTS AND
     (APPLE OR CMAKE_SYSTEM_NAME STREQUAL "Linux") AND
     NOT FETCHCONTENT_SOURCE_DIR_MODERN_LEVELDB_REFERENCE)
    execute_process(
      COMMAND
        "${CMAKE_COMMAND}"
        "-DLEVELDB_SOURCE_DIR=${modern_leveldb_reference_SOURCE_DIR}"
        -P "${reference_control_patch}"
      RESULT_VARIABLE patch_status
    )
    if(NOT patch_status EQUAL 0)
      message(FATAL_ERROR "Failed to expose the pinned LevelDB mmap-limit control")
    endif()
    set(reference_pread_control TRUE)
  endif()
  add_subdirectory(
    "${modern_leveldb_reference_SOURCE_DIR}"
    "${modern_leveldb_reference_BINARY_DIR}"
    EXCLUDE_FROM_ALL
    SYSTEM
  )
  # Upstream probes system libraries and links bare names; use the verified targets.
  set_property(
    TARGET leveldb PROPERTY LINK_LIBRARIES
    "${MODERN_LEVELDB_SNAPPY_TARGET};${MODERN_LEVELDB_ZSTD_TARGET};Threads::Threads"
  )
  set_property(
    TARGET leveldb PROPERTY INTERFACE_LINK_LIBRARIES
    "${MODERN_LEVELDB_SNAPPY_TARGET};${MODERN_LEVELDB_ZSTD_TARGET};Threads::Threads"
  )
  get_target_property(reference_link_libraries leveldb LINK_LIBRARIES)
  list(FIND reference_link_libraries "${MODERN_LEVELDB_CRC32C_TARGET}" reference_crc32c_index)
  if(reference_crc32c_index EQUAL -1)
    set(reference_crc32c_linked "false")
  else()
    set(reference_crc32c_linked "true")
  endif()
  if(HAVE_CRC32C)
    set(reference_have_crc32c "true")
  else()
    set(reference_have_crc32c "false")
  endif()
  set(reference_hardware_patch_sha256 "not_applicable")
  if(DEFINED MODERN_LEVELDB_REFERENCE_HARDWARE_PATCH_SHA256)
    string(LENGTH "${MODERN_LEVELDB_REFERENCE_HARDWARE_PATCH_SHA256}" patch_sha256_length)
    if(NOT patch_sha256_length EQUAL 64 OR
       NOT MODERN_LEVELDB_REFERENCE_HARDWARE_PATCH_SHA256 MATCHES "^[0-9a-f]+$")
      message(FATAL_ERROR "Reference hardware patch SHA-256 must contain 64 lowercase hex digits")
    endif()
    set(
      reference_hardware_patch_sha256
      "${MODERN_LEVELDB_REFERENCE_HARDWARE_PATCH_SHA256}"
    )
  endif()
  # Keep reference interfaces compatible with RTTI-enabled tests and UBSan vptr checks.
  target_compile_options(
    leveldb PRIVATE
      "$<$<CXX_COMPILER_ID:GNU,Clang,AppleClang>:-frtti>"
      "$<$<CXX_COMPILER_ID:MSVC>:/GR>"
  )
  set(
    MODERN_LEVELDB_REFERENCE_SOURCE_DIR
    "${modern_leveldb_reference_SOURCE_DIR}"
    PARENT_SCOPE
  )
  set(
    MODERN_LEVELDB_REFERENCE_BINARY_DIR
    "${modern_leveldb_reference_BINARY_DIR}"
    PARENT_SCOPE
  )
  set(
    MODERN_LEVELDB_REFERENCE_PREAD_CONTROL
    "${reference_pread_control}"
    PARENT_SCOPE
  )
  set(
    MODERN_LEVELDB_REFERENCE_CONTROL_PATCH_SHA256
    "${reference_control_patch_sha256}"
    PARENT_SCOPE
  )
  set(MODERN_LEVELDB_REFERENCE_HAVE_CRC32C "${reference_have_crc32c}" PARENT_SCOPE)
  set(MODERN_LEVELDB_REFERENCE_CRC32C_LINKED "${reference_crc32c_linked}" PARENT_SCOPE)
  set(
    MODERN_LEVELDB_REFERENCE_HARDWARE_PATCH_SHA256
    "${reference_hardware_patch_sha256}"
    PARENT_SCOPE
  )
  include("${CMAKE_CURRENT_FUNCTION_LIST_DIR}/ReferenceIdentity.cmake")
  modern_leveldb_reference_identity(
    "${modern_leveldb_reference_SOURCE_DIR}"
    "${FETCHCONTENT_SOURCE_DIR_MODERN_LEVELDB_REFERENCE}"
    reference_revision reference_source reference_dirty
  )
  set(MODERN_LEVELDB_REFERENCE_REVISION "${reference_revision}" PARENT_SCOPE)
  set(MODERN_LEVELDB_REFERENCE_SOURCE_IDENTITY "${reference_source}" PARENT_SCOPE)
  set(MODERN_LEVELDB_REFERENCE_SOURCE_DIRTY "${reference_dirty}" PARENT_SCOPE)
  set(MODERN_LEVELDB_REFERENCE_WINDOWS_MMAP_CONTROL "${reference_windows_control}" PARENT_SCOPE)
  set(MODERN_LEVELDB_REFERENCE_HARDWARE_CRC "disabled" PARENT_SCOPE)
endfunction()
