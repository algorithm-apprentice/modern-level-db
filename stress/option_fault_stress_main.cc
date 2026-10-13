#include <cstdio>
#include <exception>
#include <filesystem>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "stress/option_fault_stress.h"
#include "stress_build.h"

namespace modern_leveldb::stress {
namespace {

bool Write(FILE* stream, std::string_view text) {
    return std::fwrite(text.data(), 1, text.size(), stream) == text.size();
}

int Run(std::span<const std::filesystem::path> arguments) {
    try {
        const OptionFaultStressArguments parsed = ParseOptionFaultStressArguments(arguments);
        if (parsed.help) {
            const std::string usage = OptionFaultStressUsage();
            return Write(stdout, usage) ? 0 : 1;
        }
        const StressBuildIdentity identity{
            MODERN_LEVELDB_STRESS_SOURCE_REVISION,
            MODERN_LEVELDB_STRESS_SOURCE_DIRTY != 0,
        };
        const OptionFaultStressSummary summary = RunOptionFaultStress(parsed, identity);
        const std::string completed =
            "completed option_cells=" + std::to_string(summary.option_cells) +
            " fault_scenarios=" + std::to_string(summary.fault_scenarios) +
            " operations_per_cell=" + std::to_string(summary.operations_per_cell) +
            " liveness_writes=" + std::to_string(summary.liveness.writes) + "\n";
        return Write(stdout, completed) ? 0 : 1;
    } catch (const std::invalid_argument& error) {
        const std::string message = std::string(error.what()) + "\n" + OptionFaultStressUsage();
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
