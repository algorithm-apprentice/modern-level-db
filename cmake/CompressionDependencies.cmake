include(FetchContent)

macro(modern_leveldb_set_temporary_cache_default variable type value description)
  get_property(_modern_leveldb_had_cache_entry CACHE "${variable}" PROPERTY TYPE SET)
  if(NOT _modern_leveldb_had_cache_entry)
    if(DEFINED ${variable})
      set(_modern_leveldb_dependency_value "${${variable}}")
    else()
      set(_modern_leveldb_dependency_value "${value}")
    endif()
    set(
      "${variable}"
      "${_modern_leveldb_dependency_value}"
      CACHE "${type}" "${description}" FORCE
    )
    list(APPEND _modern_leveldb_temporary_cache_variables "${variable}")
  endif()
endmacro()

macro(modern_leveldb_remove_temporary_cache_defaults)
  foreach(_modern_leveldb_variable IN LISTS _modern_leveldb_temporary_cache_variables)
    unset("${_modern_leveldb_variable}" CACHE)
  endforeach()
  unset(_modern_leveldb_temporary_cache_variables)
endmacro()

function(modern_leveldb_compression_dependencies snappy_output zstd_output)
  if(TARGET Snappy::snappy)
    set(_modern_leveldb_snappy_target Snappy::snappy)
  elseif(TARGET snappy)
    set(_modern_leveldb_snappy_target snappy)
  else()
    set(_modern_leveldb_temporary_cache_variables)
    modern_leveldb_set_temporary_cache_default(
      BUILD_SHARED_LIBS BOOL OFF "Build shared libraries"
    )
    modern_leveldb_set_temporary_cache_default(
      SNAPPY_BUILD_TESTS BOOL OFF "Build Snappy tests"
    )
    modern_leveldb_set_temporary_cache_default(
      SNAPPY_BUILD_BENCHMARKS BOOL OFF "Build Snappy benchmarks"
    )
    modern_leveldb_set_temporary_cache_default(
      SNAPPY_INSTALL BOOL OFF "Install Snappy"
    )
    FetchContent_Declare(
      modern_leveldb_snappy
      URL
        "https://github.com/google/snappy/archive/9c28114a38866f6deeaa826db918293bc28ae410.tar.gz"
      DOWNLOAD_EXTRACT_TIMESTAMP TRUE
      SYSTEM
    )
    FetchContent_MakeAvailable(modern_leveldb_snappy)
    set_property(DIRECTORY "${modern_leveldb_snappy_SOURCE_DIR}" PROPERTY EXCLUDE_FROM_ALL TRUE)
    modern_leveldb_remove_temporary_cache_defaults()
    if(NOT TARGET snappy)
      message(FATAL_ERROR "The pinned Snappy source did not define target snappy")
    endif()
    set(_modern_leveldb_snappy_target snappy)
  endif()

  foreach(
    _modern_leveldb_candidate
    IN ITEMS
      zstd::libzstd_static
      zstd::libzstd_shared
      libzstd_static
      libzstd_shared
      libzstd
  )
    if(TARGET "${_modern_leveldb_candidate}")
      set(_modern_leveldb_zstd_target "${_modern_leveldb_candidate}")
      break()
    endif()
  endforeach()
  if(NOT _modern_leveldb_zstd_target)
    get_property(
      _modern_leveldb_had_build_type
      CACHE CMAKE_BUILD_TYPE
      PROPERTY TYPE
      SET
    )
    if(_modern_leveldb_had_build_type)
      get_property(
        _modern_leveldb_build_type_type
        CACHE CMAKE_BUILD_TYPE
        PROPERTY TYPE
      )
      get_property(
        _modern_leveldb_build_type_help
        CACHE CMAKE_BUILD_TYPE
        PROPERTY HELPSTRING
      )
      get_property(
        _modern_leveldb_build_type_value
        CACHE CMAKE_BUILD_TYPE
        PROPERTY VALUE
      )
    endif()

    set(_modern_leveldb_temporary_cache_variables)
    modern_leveldb_set_temporary_cache_default(
      ZSTD_LEGACY_SUPPORT BOOL OFF "Build Zstd legacy decoders"
    )
    modern_leveldb_set_temporary_cache_default(
      ZSTD_MULTITHREAD_SUPPORT BOOL OFF "Build Zstd multithreading support"
    )
    modern_leveldb_set_temporary_cache_default(
      ZSTD_BUILD_PROGRAMS BOOL OFF "Build Zstd programs"
    )
    modern_leveldb_set_temporary_cache_default(
      ZSTD_BUILD_CONTRIB BOOL OFF "Build Zstd contributed programs"
    )
    modern_leveldb_set_temporary_cache_default(
      ZSTD_BUILD_TESTS BOOL OFF "Build Zstd tests"
    )
    modern_leveldb_set_temporary_cache_default(
      ZSTD_BUILD_STATIC BOOL ON "Build the static Zstd library"
    )
    set(_modern_leveldb_shared_default OFF)
    if(NOT ZSTD_BUILD_STATIC)
      set(_modern_leveldb_shared_default ON)
    endif()
    modern_leveldb_set_temporary_cache_default(
      ZSTD_BUILD_SHARED BOOL "${_modern_leveldb_shared_default}" "Build the shared Zstd library"
    )
    modern_leveldb_set_temporary_cache_default(
      ZSTD_BUILD_DICTBUILDER BOOL OFF "Build the Zstd dictionary builder"
    )
    modern_leveldb_set_temporary_cache_default(
      ZSTD_BUILD_DEPRECATED BOOL OFF "Build deprecated Zstd APIs"
    )
    FetchContent_Declare(
      modern_leveldb_zstd
      URL
        "https://github.com/facebook/zstd/archive/f8745da6ff1ad1e7bab384bd1f9d742439278e99.tar.gz"
      DOWNLOAD_EXTRACT_TIMESTAMP TRUE
      SOURCE_SUBDIR build/cmake
      SYSTEM
    )
    FetchContent_MakeAvailable(modern_leveldb_zstd)
    set_property(
      DIRECTORY "${modern_leveldb_zstd_SOURCE_DIR}/build/cmake"
      PROPERTY EXCLUDE_FROM_ALL TRUE
    )
    modern_leveldb_remove_temporary_cache_defaults()

    if(_modern_leveldb_had_build_type)
      set(
        CMAKE_BUILD_TYPE
        "${_modern_leveldb_build_type_value}"
        CACHE "${_modern_leveldb_build_type_type}"
        "${_modern_leveldb_build_type_help}"
        FORCE
      )
    else()
      unset(CMAKE_BUILD_TYPE CACHE)
    endif()

    foreach(
      _modern_leveldb_candidate
      IN ITEMS libzstd_static libzstd_shared libzstd
    )
      if(TARGET "${_modern_leveldb_candidate}")
        set(_modern_leveldb_zstd_target "${_modern_leveldb_candidate}")
        break()
      endif()
    endforeach()
    if(NOT _modern_leveldb_zstd_target)
      message(FATAL_ERROR "The pinned Zstd source did not define a library target")
    endif()
  endif()

  set("${snappy_output}" "${_modern_leveldb_snappy_target}" PARENT_SCOPE)
  set("${zstd_output}" "${_modern_leveldb_zstd_target}" PARENT_SCOPE)
  if(DEFINED modern_leveldb_snappy_SOURCE_DIR)
    set(
      MODERN_LEVELDB_SNAPPY_SOURCE_DIR
      "${modern_leveldb_snappy_SOURCE_DIR}"
      PARENT_SCOPE
    )
  endif()
  if(DEFINED modern_leveldb_zstd_SOURCE_DIR)
    set(
      MODERN_LEVELDB_ZSTD_SOURCE_DIR
      "${modern_leveldb_zstd_SOURCE_DIR}"
      PARENT_SCOPE
    )
  endif()
endfunction()
