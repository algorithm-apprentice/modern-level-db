#ifndef MODERN_LEVELDB_TEST_SUPPORT_NATIVE_FILE_SYSTEM_H_
#define MODERN_LEVELDB_TEST_SUPPORT_NATIVE_FILE_SYSTEM_H_

#if defined(_WIN32)
#include "platform/windows_file_system.h"
#else
#include "platform/posix_file_system.h"
#endif

namespace modern_leveldb::test_support {

// Native fixtures consent explicitly to Windows' weaker namespace policy.
[[nodiscard]] inline auto CopiedFileSystem() {
#if defined(_WIN32)
  return WindowsFileSystem(true);
#else
  return PosixFileSystem(false);
#endif
}

}  // namespace modern_leveldb::test_support

#endif  // MODERN_LEVELDB_TEST_SUPPORT_NATIVE_FILE_SYSTEM_H_
