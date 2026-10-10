#include "diagnostics/windows_output.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "diagnostics/dump_command.h"
#include "format/write_batch.h"
#include "platform/windows_file_system.h"
#include "support/temporary_directory.h"
#include "wal/wal_io.h"

namespace modern_leveldb {
namespace {

struct HandleCloser {
    void operator()(void* handle) const noexcept {
        if (handle != INVALID_HANDLE_VALUE) {
            static_cast<void>(::CloseHandle(handle));
        }
    }
};
using Handle = std::unique_ptr<void, HandleCloser>;

class OutputOperations final : public WindowsFileOperations {
public:
    bool zero = false;
    mutable unsigned writes = 0;
    BOOL Write(HANDLE file, const void* bytes, DWORD size, DWORD* written) const noexcept override {
        ++writes;
        if (zero) {
            *written = 0;
            ::SetLastError(ERROR_ACCESS_DENIED);
            return TRUE;
        }
        return WindowsFileOperations::Write(file, bytes, std::min<DWORD>(size, 2), written);
    }
};

TEST(WindowsOutputTest, WritesExactLfBytesAndNeverClosesABorrowedPipe) {
    HANDLE raw_read = nullptr;
    HANDLE raw_write = nullptr;
    ASSERT_TRUE(::CreatePipe(&raw_read, &raw_write, nullptr, 0));
    Handle read(raw_read);
    Handle write(raw_write);
    {
        WindowsOutputFile output(write.get());
        ASSERT_TRUE(output.Append(AsBytes("first\nsecond\n")).has_value());
        ASSERT_TRUE(output.Flush().has_value());
        ASSERT_TRUE(output.Sync().has_value());
        ASSERT_TRUE(output.Close().has_value());
        EXPECT_EQ(output.Append(AsBytes("later")).error().code(), ErrorCode::InvalidArgument);
    }
    DWORD flags = 0;
    EXPECT_TRUE(::GetHandleInformation(write.get(), &flags));
    std::array<std::byte, 32> bytes{};
    DWORD received = 0;
    ASSERT_TRUE(
        ::ReadFile(read.get(), bytes.data(), static_cast<DWORD>(bytes.size()), &received, nullptr));
    EXPECT_EQ(AsStringView(ByteView(bytes).first(received)), "first\nsecond\n");
}

TEST(WindowsOutputTest, ClosedPipeAndAbsentHandleAreTypedFailuresNotStaleErrors) {
    HANDLE raw_read = nullptr;
    HANDLE raw_write = nullptr;
    ASSERT_TRUE(::CreatePipe(&raw_read, &raw_write, nullptr, 0));
    Handle read(raw_read);
    Handle write(raw_write);
    read.reset();
    DWORD native_written = 0;
    ASSERT_FALSE(::WriteFile(write.get(), "probe", 5, &native_written, nullptr));
    const DWORD native_error = ::GetLastError();
    EXPECT_TRUE(native_error == ERROR_BROKEN_PIPE || native_error == ERROR_NO_DATA);
    WindowsOutputFile output(write.get());
    const auto failed = output.Append(AsBytes("data"));
    ASSERT_FALSE(failed.has_value());
    EXPECT_EQ(failed.error().code(), ErrorCode::Io);
    EXPECT_NE(failed.error().message().find("Win32 " + std::to_string(native_error)),
              std::string_view::npos);
    ::SetLastError(ERROR_ACCESS_DENIED);
    WindowsOutputFile absent(nullptr);
    const auto missing = absent.Append(AsBytes("data"));
    ASSERT_FALSE(missing.has_value());
    EXPECT_EQ(missing.error().code(), ErrorCode::Io);
    EXPECT_EQ(missing.error().message().find("Win32 5"), std::string_view::npos);
}

TEST(WindowsOutputTest, ShortAndZeroProgressWritesHaveExactCompleteOrFailureSemantics) {
    HANDLE raw_read = nullptr;
    HANDLE raw_write = nullptr;
    ASSERT_TRUE(::CreatePipe(&raw_read, &raw_write, nullptr, 0));
    Handle read(raw_read);
    Handle write(raw_write);
    auto operations = std::make_shared<OutputOperations>();
    WindowsOutputFile output(write.get(), operations);
    ASSERT_TRUE(output.Append(AsBytes("short\n")).has_value());
    EXPECT_EQ(operations->writes, 3U);
    std::array<std::byte, 6> bytes{};
    DWORD received = 0;
    ASSERT_TRUE(
        ::ReadFile(read.get(), bytes.data(), static_cast<DWORD>(bytes.size()), &received, nullptr));
    EXPECT_EQ(AsStringView(bytes), "short\n");
    operations->zero = true;
    const auto failed = output.Append(AsBytes("later"));
    ASSERT_FALSE(failed.has_value());
    EXPECT_EQ(failed.error().code(), ErrorCode::Io);
    EXPECT_EQ(failed.error().message(), "write output made no progress");
}

class StringOutput final : public WritableFile {
public:
    Status Append(ByteView bytes) override {
        text.append(AsStringView(bytes));
        return {};
    }
    Status Flush() override { return {}; }
    Status Sync() override { return {}; }
    Status Close() override { return {}; }
    std::string text;
};

TEST(NativeDumpCommandTest, PreservesWideUnicodePathsAndInputBytes) {
    test_support::TemporaryDirectory directory;
    const auto native = directory.path() / std::filesystem::path{u8"\u6570\u636e-\U0001f4be"};
    WindowsFileSystem file_system;
    ASSERT_TRUE(file_system.CreateDirectory(native).has_value());
    const auto path = native / "000001.log";
    EncodedWriteBatch batch;
    ASSERT_TRUE(batch.Put(AsBytes("key"), AsBytes("value")).has_value());
    ASSERT_TRUE(batch.SetSequence(1).has_value());
    auto writable = file_system.OpenWritable(path);
    ASSERT_TRUE(writable.has_value());
    WalWriter writer(std::move(*writable));
    ASSERT_TRUE(writer.AddRecord(batch.encoded()).has_value());
    ASSERT_TRUE(writer.Close().has_value());
    auto input = file_system.OpenSequential(path);
    ASSERT_TRUE(input.has_value());
    std::vector<std::byte> before(static_cast<std::size_t>(file_system.FileSize(path).value()));
    ASSERT_EQ((*input)->Read(before).value(), before.size());
    input->reset();
    const auto names_before = file_system.ListDirectory(native).value();
    StringOutput output;
    StringOutput errors;
    const std::array<std::filesystem::path, 2> arguments{std::filesystem::path{"dump"}, path};
    EXPECT_EQ(RunNativeDiagnosticTool(arguments, file_system, output, errors), 0);
    EXPECT_TRUE(errors.text.empty());
    EXPECT_NE(output.text.find("\\xe6\\x95\\xb0\\xe6\\x8d\\xae"), std::string::npos);
    EXPECT_NE(output.text.find("key='key' value='value'"), std::string::npos);
    auto after_file = file_system.OpenSequential(path);
    ASSERT_TRUE(after_file.has_value());
    std::vector<std::byte> after(before.size());
    ASSERT_EQ((*after_file)->Read(after).value(), after.size());
    EXPECT_EQ(after, before);
    EXPECT_EQ(file_system.ListDirectory(native).value(), names_before);
}

TEST(NativeDumpCommandTest, HelpDoesNotUseAnUnavailableErrorSink) {
    WindowsFileSystem file_system;
    StringOutput output;
    WindowsOutputFile unavailable(nullptr);
    const std::array<std::filesystem::path, 1> arguments{std::filesystem::path{"--help"}};
    EXPECT_EQ(RunNativeDiagnosticTool(arguments, file_system, output, unavailable), 0);
    EXPECT_EQ(output.text, "Usage: modern_leveldb_tool dump FILE...\n");
}

}  // namespace
}  // namespace modern_leveldb
