#include <string_view>

#include "platform/windows_file_system.h"

int wmain(int argc, wchar_t* argv[]) {
    if (argc != 3) {
        return 2;
    }
    const bool expect_busy = std::wstring_view(argv[1]) == L"busy";
    if (!expect_busy && std::wstring_view(argv[1]) != L"free") {
        return 2;
    }
    modern_leveldb::WindowsFileSystem file_system;
    const auto lock = file_system.LockFile(argv[2]);
    if (expect_busy) {
        return !lock.has_value() && lock.error().code() == modern_leveldb::ErrorCode::Busy ? 0 : 1;
    }
    return lock.has_value() ? 0 : 1;
}
