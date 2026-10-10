#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>

#include "modern_leveldb/db.h"
#include "support/windows_crash_process.h"

#ifndef MODERN_LEVELDB_WINDOWS_CRASH_CHILD
#error MODERN_LEVELDB_WINDOWS_CRASH_CHILD must name the native crash child executable
#endif

namespace modern_leveldb {
namespace {

using namespace std::chrono_literals;
using test_support::WindowsCrashProcess;

class CrashDirectory final {
public:
    CrashDirectory() {
        static std::atomic<unsigned> next{0};
        const auto stamp = std::chrono::steady_clock::now().time_since_epoch().count();
        do {
            path_ =
                std::filesystem::current_path() / ("modern-leveldb-crash-" + std::to_string(stamp) +
                                                   "-" + std::to_string(next.fetch_add(1)));
        } while (!std::filesystem::create_directory(path_));
    }

    CrashDirectory(const CrashDirectory&) = delete;
    CrashDirectory& operator=(const CrashDirectory&) = delete;
    ~CrashDirectory() {
        std::error_code error;
        std::filesystem::remove_all(path_, error);
        if (error) {
            std::fprintf(stderr, "Windows crash directory cleanup failed (error %d)\n",
                         error.value());
        }
    }

    [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

private:
    std::filesystem::path path_;
};

Result<std::unique_ptr<WindowsCrashProcess>> StartChild(std::wstring_view mode,
                                                        const std::filesystem::path& directory) {
    return WindowsCrashProcess::Start(std::filesystem::path{MODERN_LEVELDB_WINDOWS_CRASH_CHILD},
                                      mode, directory);
}

void VerifyRecoveredWrites(const std::filesystem::path& path) {
    Options options;
    options.allow_weak_namespace_durability = true;
    auto database = Database::Open(options, path);
    ASSERT_TRUE(database.has_value()) << database.error().ToString();
    const std::string expected(80 * 1'024, 'x');
    for (unsigned index = 0; index < 8; ++index) {
        const auto value = database->Get(AsBytes("key-" + std::to_string(index)));
        ASSERT_TRUE(value.has_value()) << value.error().ToString();
        ASSERT_TRUE(value->has_value());
        EXPECT_EQ(AsStringView(**value), expected);
    }
}

class WindowsProcessCrashRecoveryTest : public ::testing::TestWithParam<bool> {
protected:
    std::filesystem::path DatabasePath(const CrashDirectory& directory) const {
        return directory.path() / (GetParam() ? L"db \u6570\u636E \u03BB" : L"db with spaces");
    }
};

TEST_P(WindowsProcessCrashRecoveryTest, ExitProcessRecoversAllAcknowledgedWrites) {
    CrashDirectory directory;
    const auto path = DatabasePath(directory);
    auto started = StartChild(L"exit", path);
    ASSERT_TRUE(started.has_value()) << started.error().ToString();
    auto child = std::move(*started);

    const Status ready = child->WaitUntilReady(30s);
    ASSERT_TRUE(ready.has_value()) << ready.error().ToString();
    const Status completed = child->Complete(10s);
    ASSERT_TRUE(completed.has_value()) << completed.error().ToString();
    const auto running = child->Running();
    ASSERT_TRUE(running.has_value()) << running.error().ToString();
    EXPECT_FALSE(*running);
    const auto exit_code = child->ExitCode();
    ASSERT_TRUE(exit_code.has_value()) << exit_code.error().ToString();
    EXPECT_EQ(*exit_code, 0U);

    VerifyRecoveredWrites(path);
}

TEST_P(WindowsProcessCrashRecoveryTest, OwnedTerminationReleasesLockAndRecoversAllWrites) {
    CrashDirectory directory;
    const auto path = DatabasePath(directory);
    auto started = StartChild(L"hold", path);
    ASSERT_TRUE(started.has_value()) << started.error().ToString();
    auto child = std::move(*started);

    const Status ready = child->WaitUntilReady(30s);
    ASSERT_TRUE(ready.has_value()) << ready.error().ToString();
    const auto running = child->Running();
    ASSERT_TRUE(running.has_value()) << running.error().ToString();
    ASSERT_TRUE(*running);
    const auto live_exit_code = child->ExitCode();
    ASSERT_FALSE(live_exit_code.has_value());
    EXPECT_EQ(live_exit_code.error().code(), ErrorCode::Busy);

    Options options;
    options.allow_weak_namespace_durability = true;
    const auto conflicting = Database::Open(options, path);
    ASSERT_FALSE(conflicting.has_value());
    EXPECT_EQ(conflicting.error().code(), ErrorCode::Busy);

    const Status completed = child->TerminateAndComplete(5s);
    ASSERT_TRUE(completed.has_value()) << completed.error().ToString();
    const auto stopped = child->Running();
    ASSERT_TRUE(stopped.has_value()) << stopped.error().ToString();
    EXPECT_FALSE(*stopped);
    const auto exit_code = child->ExitCode();
    ASSERT_TRUE(exit_code.has_value()) << exit_code.error().ToString();
    EXPECT_EQ(*exit_code, 1U);

    VerifyRecoveredWrites(path);
}

INSTANTIATE_TEST_SUITE_P(NativePaths, WindowsProcessCrashRecoveryTest, ::testing::Bool(),
                         [](const ::testing::TestParamInfo<bool>& information) {
                             return information.param ? "Unicode" : "Ascii";
                         });

TEST(WindowsProcessCrashTest, MalformedObservationIsRejectedAndChildIsReaped) {
    CrashDirectory directory;
    auto started = StartChild(L"malformed", directory.path() / L"db");
    ASSERT_TRUE(started.has_value()) << started.error().ToString();
    auto child = std::move(*started);
    const Status ready = child->WaitUntilReady(30s);
    ASSERT_FALSE(ready.has_value());
    const Status completed = child->Complete(10s);
    ASSERT_FALSE(completed.has_value());
    EXPECT_EQ(completed.error().code(), ready.error().code());
    EXPECT_EQ(completed.error().message(), ready.error().message());
    const auto running = child->Running();
    ASSERT_TRUE(running.has_value()) << running.error().ToString();
    EXPECT_FALSE(*running);
    const auto exit_code = child->ExitCode();
    ASSERT_TRUE(exit_code.has_value()) << exit_code.error().ToString();
    EXPECT_NE(*exit_code, 0U);
}

TEST(WindowsProcessCrashTest, ValidReadinessCannotHideInvalidTrailingOutput) {
    CrashDirectory directory;
    auto started = StartChild(L"tail", directory.path() / L"db with spaces");
    ASSERT_TRUE(started.has_value()) << started.error().ToString();
    auto child = std::move(*started);
    const Status ready = child->WaitUntilReady(30s);
    ASSERT_TRUE(ready.has_value()) << ready.error().ToString();
    const Status completed = child->Complete(10s);
    ASSERT_FALSE(completed.has_value());
    EXPECT_EQ(completed.error().code(), ErrorCode::Corruption);
    const auto running = child->Running();
    ASSERT_TRUE(running.has_value()) << running.error().ToString();
    EXPECT_FALSE(*running);
    const auto exit_code = child->ExitCode();
    ASSERT_TRUE(exit_code.has_value()) << exit_code.error().ToString();
    EXPECT_EQ(*exit_code, 0U);
}

TEST(WindowsProcessCrashTest, ReadinessTimeoutStaysFailedAfterContainedCleanup) {
    CrashDirectory directory;
    auto started = StartChild(L"stall", directory.path() / L"db");
    ASSERT_TRUE(started.has_value()) << started.error().ToString();
    auto child = std::move(*started);
    const Status ready = child->WaitUntilReady(100ms);
    ASSERT_FALSE(ready.has_value());
    EXPECT_EQ(ready.error().code(), ErrorCode::Aborted);
    const auto running = child->Running();
    ASSERT_TRUE(running.has_value()) << running.error().ToString();
    ASSERT_TRUE(*running);
    const Status completed = child->TerminateAndComplete(5s);
    ASSERT_FALSE(completed.has_value());
    EXPECT_EQ(completed.error().code(), ready.error().code());
    EXPECT_EQ(completed.error().message(), ready.error().message());
    const auto stopped = child->Running();
    ASSERT_TRUE(stopped.has_value()) << stopped.error().ToString();
    EXPECT_FALSE(*stopped);
    const auto exit_code = child->ExitCode();
    ASSERT_TRUE(exit_code.has_value()) << exit_code.error().ToString();
    EXPECT_EQ(*exit_code, 1U);
}

TEST(WindowsProcessCrashTest, DestructionContainsAnUncompletedHeldDatabase) {
    CrashDirectory directory;
    const auto path = directory.path() / L"db";
    auto started = StartChild(L"hold", path);
    ASSERT_TRUE(started.has_value()) << started.error().ToString();
    auto child = std::move(*started);
    const Status ready = child->WaitUntilReady(30s);
    ASSERT_TRUE(ready.has_value()) << ready.error().ToString();

    const auto before = std::chrono::steady_clock::now();
    child.reset();
    EXPECT_LT(std::chrono::steady_clock::now() - before, 10s);
    Options options;
    options.allow_weak_namespace_durability = true;
    const auto reopened = Database::Open(options, path);
    ASSERT_TRUE(reopened.has_value()) << reopened.error().ToString();
}

TEST(WindowsProcessCrashTest, InvalidExecutableReportsNativeLaunchFailure) {
    CrashDirectory directory;
    const auto started = WindowsCrashProcess::Start(directory.path() / L"missing-child.exe",
                                                    L"exit", directory.path() / L"db");
    ASSERT_FALSE(started.has_value());
    EXPECT_EQ(started.error().code(), ErrorCode::Io);
    EXPECT_NE(started.error().message().find("CreateProcessW"), std::string_view::npos);
    EXPECT_NE(started.error().message().find("Win32 2"), std::string_view::npos);
}

}  // namespace
}  // namespace modern_leveldb
