#include "stress/stress.h"

#include <cstdio>
#include <exception>
#include <filesystem>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "stress_build.h"

namespace modern_leveldb::stress {
namespace {

bool Write(FILE* stream, std::string_view text) {
    return std::fwrite(text.data(), 1, text.size(), stream) == text.size();
}

int Run(std::span<const std::filesystem::path> arguments) {
    try {
        const StressArguments parsed = ParseStressArguments(arguments);
        if (parsed.help) {
            const std::string usage = StressUsage();
            return Write(stdout, usage) ? 0 : 1;
        }
        const StressBuildIdentity identity{
            MODERN_LEVELDB_STRESS_SOURCE_REVISION,
            MODERN_LEVELDB_STRESS_SOURCE_DIRTY != 0,
        };
        const StressSummary summary = RunStress(parsed, identity);
        const std::string completed =
            "completed operations=" + std::to_string(summary.operations) +
            " entries=" + std::to_string(summary.entries) + "\n";
        return Write(stdout, completed) ? 0 : 1;
    } catch (const std::invalid_argument& error) {
        const std::string message = std::string(error.what()) + "\n" + StressUsage();
        static_cast<void>(Write(stderr, message));
        return 2;
    } catch (const std::exception& error) {
        const std::string message = std::string(error.what()) + "\n";
        static_cast<void>(Write(stderr, message));
        return 1;
    }
}

}  // namespace
}  // namespace modern_leveldb::stress

#if defined(_WIN32)
int wmain(int argc, wchar_t* argv[]) {
    std::vector<std::filesystem::path> arguments;
    for (int index = 1; index < argc; ++index) {
        arguments.emplace_back(argv[index]);
    }
    return modern_leveldb::stress::Run(arguments);
}
#else
int main(int argc, char* argv[]) {
    std::vector<std::filesystem::path> arguments;
    for (int index = 1; index < argc; ++index) {
        arguments.emplace_back(argv[index]);
    }
    return modern_leveldb::stress::Run(arguments);
}
#endif
