#include <string_view>
#include <vector>

#include "diagnostics/dump_command.h"
#if defined(_WIN32)
#include "diagnostics/windows_output.h"
#include "platform/windows_file_system.h"

int wmain(int argc, wchar_t* argv[]) {
    modern_leveldb::WindowsFileSystem file_system;
    auto output = modern_leveldb::WindowsOutputFile::Standard(STD_OUTPUT_HANDLE);
    auto errors = modern_leveldb::WindowsOutputFile::Standard(STD_ERROR_HANDLE);
    std::vector<std::filesystem::path> arguments;
    arguments.reserve(static_cast<std::size_t>(argc - 1));
    for (int index = 1; index < argc; ++index) {
        arguments.emplace_back(argv[index]);
    }
    return modern_leveldb::RunNativeDiagnosticTool(arguments, file_system, output, errors);
}
#else
#include <unistd.h>

#include "diagnostics/posix_output.h"
#include "platform/posix_file_system.h"

int main(int argc, char* argv[]) {
    if (!modern_leveldb::IgnoreBrokenPipeSignal().has_value()) {
        return 1;
    }
    modern_leveldb::PosixFileSystem file_system(false);
    modern_leveldb::PosixOutputFile output(STDOUT_FILENO);
    modern_leveldb::PosixOutputFile errors(STDERR_FILENO);
    std::vector<std::string_view> arguments;
    arguments.reserve(static_cast<std::size_t>(argc - 1));
    for (int index = 1; index < argc; ++index) {
        arguments.emplace_back(argv[index]);
    }
    return modern_leveldb::RunDiagnosticTool(arguments, file_system, output, errors);
}
#endif
