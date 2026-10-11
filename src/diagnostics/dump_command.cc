#include "diagnostics/dump_command.h"

#include <cstddef>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>

#include "diagnostics/dump_file.h"
#include "modern_leveldb/base/bytes.h"
#include "modern_leveldb/base/result.h"
#include "platform/file_system.h"

namespace modern_leveldb {
namespace {

constexpr std::string_view Usage = "Usage: modern_leveldb_tool dump FILE...";

class TrackingOutput final : public WritableFile {
public:
    explicit TrackingOutput(WritableFile& output) noexcept : output_(&output) {}

    Status Append(ByteView data) override {
        const Status status = output_->Append(data);
        if (!status.has_value()) {
            failed_ = true;
        }
        return status;
    }
    // GCOVR_EXCL_START: DumpFile's borrowed output contract calls only Append
    Status Flush() override { return output_->Flush(); }
    Status Sync() override { return output_->Sync(); }
    Status Close() override { return output_->Close(); }
    // GCOVR_EXCL_STOP

    [[nodiscard]] bool failed() const noexcept { return failed_; }

private:
    WritableFile* output_;
    bool failed_ = false;
};

Status WriteUsage(WritableFile& output) { return AppendDiagnosticLine(output, std::string(Usage)); }

Status ReportError(WritableFile& errors, const std::filesystem::path& path, const Error& error) {
    Result<std::string> escaped_path = EscapeDiagnosticPath(path);
    std::string line = "file='";
    line += escaped_path.value_or("<unrepresentable>");
    line += "' error='";
    const std::string error_text = error.ToString();
    line += EscapeDiagnosticBytes(AsBytes(error_text));
    line.push_back('\'');
    return AppendDiagnosticLine(errors, std::move(line));
}

bool Matches(std::string_view argument, std::string_view command) noexcept {
    return argument == command;
}
bool Matches(const std::filesystem::path& argument, std::string_view command) {
    return argument == std::filesystem::path(command);
}
bool IsOption(std::string_view argument) noexcept { return argument.starts_with('-'); }
bool IsOption(const std::filesystem::path& argument) noexcept {
    const auto& native = argument.native();
    return !native.empty() && native.front() == static_cast<std::filesystem::path::value_type>('-');
}

enum class CommandKind {
    Help,
    Invalid,
    Dump,
};

template <typename Argument>
bool ContainsOptions(std::span<const Argument> files) {
    for (const auto& argument : files) {
        if (IsOption(argument)) {
            return true;
        }
    }
    return false;
}

template <typename Argument>
CommandKind ClassifyCommand(std::span<const Argument> arguments) {
    if (arguments.size() == 1 && Matches(arguments.front(), "--help")) {
        return CommandKind::Help;
    }
    if (arguments.size() < 2 || !Matches(arguments.front(), "dump")) {
        return CommandKind::Invalid;
    }
    if (ContainsOptions(arguments.subspan(1))) {
        return CommandKind::Invalid;
    }
    return CommandKind::Dump;
}

enum class FileOutcome {
    Clean,
    InputError,
    OutputError,
};

FileOutcome DumpCommandFile(const std::filesystem::path& path, FileSystem& file_system,
                            TrackingOutput& output, WritableFile& errors) {
    const Status dumped = DumpFile(file_system, path, output);
    if (dumped.has_value()) {
        return FileOutcome::Clean;
    }
    const Status reported = ReportError(errors, path, dumped.error());
    if (!reported.has_value()) {
        return FileOutcome::OutputError;
    }
    if (output.failed()) {
        return FileOutcome::OutputError;
    }
    return FileOutcome::InputError;
}

template <typename Argument>
int DumpFiles(std::span<const Argument> files, FileSystem& file_system, WritableFile& output,
              WritableFile& errors) {
    TrackingOutput tracked(output);
    bool failed = false;
    for (const auto& argument : files) {
        const std::filesystem::path path(argument);
        const FileOutcome outcome = DumpCommandFile(path, file_system, tracked, errors);
        if (outcome == FileOutcome::OutputError) {
            return 1;
        }
        if (outcome == FileOutcome::InputError) {
            failed = true;
        }
    }
    return failed ? 1 : 0;
}

template <typename Argument>
int RunCommand(std::span<const Argument> arguments, FileSystem& file_system, WritableFile& output,
               WritableFile& errors) {
    const CommandKind kind = ClassifyCommand(arguments);
    if (kind == CommandKind::Help) {
        return WriteUsage(output).has_value() ? 0 : 1;
    }
    if (kind == CommandKind::Invalid) {
        return WriteUsage(errors).has_value() ? 2 : 1;
    }
    return DumpFiles(arguments.subspan(1), file_system, output, errors);
}

}  // namespace

int RunDiagnosticTool(std::span<const std::string_view> arguments, FileSystem& file_system,
                      WritableFile& output, WritableFile& errors) {
    return RunCommand(arguments, file_system, output, errors);
}

int RunNativeDiagnosticTool(std::span<const std::filesystem::path> arguments,
                            FileSystem& file_system, WritableFile& output, WritableFile& errors) {
    return RunCommand(arguments, file_system, output, errors);
}

}  // namespace modern_leveldb
