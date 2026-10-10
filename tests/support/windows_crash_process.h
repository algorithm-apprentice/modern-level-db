#ifndef MODERN_LEVELDB_TESTS_SUPPORT_WINDOWS_CRASH_PROCESS_H_
#define MODERN_LEVELDB_TESTS_SUPPORT_WINDOWS_CRASH_PROCESS_H_

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string_view>

#include "modern_leveldb/base/result.h"

namespace modern_leveldb::test_support {

class WindowsCrashProcess final {
public:
    [[nodiscard]] static Result<std::unique_ptr<WindowsCrashProcess>> Start(
        const std::filesystem::path& executable, std::wstring_view mode,
        const std::filesystem::path& directory);

    WindowsCrashProcess(const WindowsCrashProcess&) = delete;
    WindowsCrashProcess& operator=(const WindowsCrashProcess&) = delete;
    ~WindowsCrashProcess();

    [[nodiscard]] Status WaitUntilReady(std::chrono::milliseconds timeout);
    [[nodiscard]] Status Complete(std::chrono::milliseconds timeout);
    [[nodiscard]] Status TerminateAndComplete(std::chrono::milliseconds timeout);
    [[nodiscard]] Result<bool> Running() const;
    [[nodiscard]] Result<std::uint32_t> ExitCode() const;

private:
    struct Impl;
    explicit WindowsCrashProcess(std::unique_ptr<Impl> impl) noexcept;

    std::unique_ptr<Impl> impl_;
};

}  // namespace modern_leveldb::test_support

#endif  // MODERN_LEVELDB_TESTS_SUPPORT_WINDOWS_CRASH_PROCESS_H_
