set(MODERN_LEVELDB_HAVE_WINDOWS_FILE_SYSTEM OFF)
if(WIN32 AND CMAKE_CXX_COMPILER_ID STREQUAL "MSVC")
  include(CheckCXXSourceCompiles)
  check_cxx_source_compiles(
    "#if !defined(_M_X64) || defined(_M_ARM64EC)\n#error The Windows backend requires native x64\n#endif\nint main() {}"
    MODERN_LEVELDB_WINDOWS_NATIVE_X64
  )
  if(MODERN_LEVELDB_WINDOWS_NATIVE_X64)
    set(MODERN_LEVELDB_HAVE_WINDOWS_FILE_SYSTEM ON)
  endif()
endif()
