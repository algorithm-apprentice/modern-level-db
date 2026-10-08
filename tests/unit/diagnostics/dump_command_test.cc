#include "diagnostics/dump_command.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "format/write_batch.h"
#include "modern_leveldb/base/bytes.h"
#include "modern_leveldb/base/result.h"
#include "platform/file_system.h"
#include "support/memory_file_system.h"
#include "wal/wal_io.h"

namespace modern_leveldb {
namespace {

using test_support::MemoryFileSystem;

class CommandOutput final : public WritableFile {
 public:
  explicit CommandOutput(std::optional<std::size_t> fail_at = std::nullopt) : fail_at_(fail_at) {}

  Status Append(ByteView data) override {
    if (fail_at_ == appends_) {
      return std::unexpected(Error::Io("command output failed"));
    }
    text_.append(AsStringView(data));
    ++appends_;
    return {};
  }
  Status Flush() override { return {}; }
  Status Sync() override { return {}; }
  Status Close() override { return {}; }

  [[nodiscard]] const std::string& text() const noexcept { return text_; }

 private:
  std::optional<std::size_t> fail_at_;
  std::size_t appends_ = 0;
  std::string text_;
};

void WriteLog(MemoryFileSystem& file_system, const std::filesystem::path& path) {
  EncodedWriteBatch batch;
  ASSERT_TRUE(batch.Put(AsBytes("a"), AsBytes("1")).has_value());
  ASSERT_TRUE(batch.SetSequence(1).has_value());
  auto file = file_system.OpenWritable(path);
  ASSERT_TRUE(file.has_value());
  WalWriter writer(std::move(*file));
  ASSERT_TRUE(writer.AddRecord(batch.encoded()).has_value());
  ASSERT_TRUE(writer.Close().has_value());
}

int RunCommand(std::initializer_list<std::string_view> arguments, MemoryFileSystem& file_system,
               CommandOutput& output, CommandOutput& errors) {
  const std::vector<std::string_view> owned(arguments);
  return RunDiagnosticTool(owned, file_system, output, errors);
}

TEST(DumpCommandTest, PrintsHelpAndUsageWithTheirExitStatuses) {
  MemoryFileSystem file_system;
  CommandOutput output;
  CommandOutput errors;

  EXPECT_EQ(RunCommand({"--help"}, file_system, output, errors), 0);
  EXPECT_EQ(output.text(), "Usage: modern_leveldb_tool dump FILE...\n");
  EXPECT_TRUE(errors.text().empty());

  CommandOutput missing_output;
  CommandOutput missing_errors;
  EXPECT_EQ(RunCommand({}, file_system, missing_output, missing_errors), 2);
  EXPECT_TRUE(missing_output.text().empty());
  EXPECT_EQ(missing_errors.text(), "Usage: modern_leveldb_tool dump FILE...\n");

  CommandOutput failed_help(0);
  CommandOutput unused_errors;
  EXPECT_EQ(RunCommand({"--help"}, file_system, failed_help, unused_errors), 1);
  CommandOutput unused_output;
  CommandOutput failed_usage(0);
  EXPECT_EQ(RunCommand({}, file_system, unused_output, failed_usage), 1);
}

TEST(DumpCommandTest, ContinuesAfterInputErrorsAndEscapesTheirPathAndMessage) {
  MemoryFileSystem file_system;
  const std::filesystem::path valid = "db/000001.log";
  WriteLog(file_system, valid);
  CommandOutput output;
  CommandOutput errors;

  EXPECT_EQ(
      RunCommand({"dump", "missing'\n/000002.log", "db/000001.log"}, file_system, output, errors),
      1);
  EXPECT_NE(errors.text().find("file='missing\\'\\n/000002.log' error='not_found:"),
            std::string::npos);
  EXPECT_NE(output.text().find("dump version=1 type=log file='db/000001.log'\n"),
            std::string::npos);
}

TEST(DumpCommandTest, StopsBeforeLaterFilesWhenStdoutFails) {
  MemoryFileSystem file_system;
  WriteLog(file_system, "000001.log");
  WriteLog(file_system, "000002.log");
  CommandOutput output(1);
  CommandOutput errors;
  const std::size_t operations_before = file_system.operations().size();

  EXPECT_EQ(RunCommand({"dump", "000001.log", "000002.log"}, file_system, output, errors), 1);
  EXPECT_NE(errors.text().find("command output failed"), std::string::npos);
  const std::vector<std::string> later_operations(
      file_system.operations().begin() + static_cast<std::ptrdiff_t>(operations_before),
      file_system.operations().end());
  EXPECT_EQ(
      std::count(later_operations.begin(), later_operations.end(), "open_sequential 000002.log"),
      0);
}

TEST(DumpCommandTest, ReturnsSuccessWhenEveryFileDumpsCleanly) {
  MemoryFileSystem file_system;
  WriteLog(file_system, "000001.log");
  WriteLog(file_system, "000002.log");
  CommandOutput output;
  CommandOutput errors;

  EXPECT_EQ(RunCommand({"dump", "000001.log", "000002.log"}, file_system, output, errors), 0);
  EXPECT_TRUE(errors.text().empty());
  EXPECT_NE(output.text().find("file='000001.log'"), std::string::npos);
  EXPECT_NE(output.text().find("file='000002.log'"), std::string::npos);
}

TEST(DumpCommandTest, RejectsUnknownCommandsAndMissingDumpFiles) {
  MemoryFileSystem file_system;
  for (const std::vector<std::string_view>& arguments :
       {std::vector<std::string_view>{"unknown", "file"}, std::vector<std::string_view>{"dump"},
        std::vector<std::string_view>{"dump", "--unknown"}}) {
    CommandOutput output;
    CommandOutput errors;
    EXPECT_EQ(RunDiagnosticTool(arguments, file_system, output, errors), 2);
    EXPECT_EQ(errors.text(), "Usage: modern_leveldb_tool dump FILE...\n");
  }
}

TEST(DumpCommandTest, StopsWhenTheErrorStreamFails) {
  MemoryFileSystem file_system;
  CommandOutput output;
  CommandOutput errors(0);

  EXPECT_EQ(RunCommand({"dump", "000001.log"}, file_system, output, errors), 1);

  CommandOutput flag_output;
  CommandOutput flag_errors(0);
  EXPECT_EQ(RunCommand({"dump", "--unknown"}, file_system, flag_output, flag_errors), 1);

  CommandOutput command_output;
  CommandOutput command_errors(0);
  EXPECT_EQ(RunCommand({"unknown", "file"}, file_system, command_output, command_errors), 1);
}

TEST(NativeCommandTest, PreservesHelpUsageAndValidatesOptionsBeforeReads) {
  MemoryFileSystem file_system;
  CommandOutput output;
  CommandOutput errors;
  const std::array<std::filesystem::path, 1> help{std::filesystem::path{"--help"}};
  EXPECT_EQ(RunNativeDiagnosticTool(help, file_system, output, errors), 0);
  EXPECT_EQ(output.text(), "Usage: modern_leveldb_tool dump FILE...\n");
  for (const auto& arguments :
       {std::vector<std::filesystem::path>{}, std::vector<std::filesystem::path>{"dump"},
        std::vector<std::filesystem::path>{"unknown", "file"},
        std::vector<std::filesystem::path>{"dump", "000001.log", "-bad"}}) {
    const auto operations = file_system.operations().size();
    EXPECT_EQ(RunNativeDiagnosticTool(arguments, file_system, output, errors), 2);
    EXPECT_EQ(file_system.operations().size(), operations);
  }
}

TEST(NativeCommandTest, SharesInputErrorContinuationAndOutputFailurePrecedence) {
  MemoryFileSystem file_system;
  WriteLog(file_system, "000001.log");
  CommandOutput output;
  CommandOutput errors;
  const std::array<std::filesystem::path, 3> arguments{std::filesystem::path{"dump"},
                                                       std::filesystem::path{"missing/000002.log"},
                                                       std::filesystem::path{"000001.log"}};
  EXPECT_EQ(RunNativeDiagnosticTool(arguments, file_system, output, errors), 1);
  EXPECT_NE(output.text().find("key='a' value='1'"), std::string::npos);
  EXPECT_NE(errors.text().find("not_found:"), std::string::npos);
  CommandOutput failed_output(0);
  CommandOutput unused_errors;
  const std::array<std::filesystem::path, 2> valid{std::filesystem::path{"dump"},
                                                   std::filesystem::path{"000001.log"}};
  EXPECT_EQ(RunNativeDiagnosticTool(valid, file_system, failed_output, unused_errors), 1);
  CommandOutput unused_output;
  CommandOutput failed_errors(0);
  EXPECT_EQ(RunNativeDiagnosticTool(arguments, file_system, unused_output, failed_errors), 1);
}

}  // namespace
}  // namespace modern_leveldb
