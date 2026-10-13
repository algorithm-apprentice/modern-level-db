#include "stress/crash_stress_worker.h"

#include <chrono>
#include <cstdio>
#include <exception>
#include <filesystem>
#include <span>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "stress_build.h"

namespace modern_leveldb::stress {
namespace {

bool Write(FILE* stream, std::string_view text) {
    return std::fwrite(text.data(), 1, text.size(), stream) == text.size() &&
           std::fflush(stream) == 0;
}

[[noreturn]] void Hold(Database& database) {
    static_cast<void>(database);
    for (;;) {
        std::this_thread::sleep_for(std::chrono::hours(24));
    }
}

int Run(std::span<const std::filesystem::path> arguments) {
    try {
        const CrashStressWorkerArguments parsed =
            ParseCrashStressWorkerArguments(arguments);
        if (parsed.help) {
            const std::string usage = CrashStressWorkerUsage();
            return Write(stdout, usage) ? 0 : 1;
        }
        switch (parsed.mode) {
            case CrashStressWorkerMode::Identity: {
                const std::string identity =
                    "identity\t" MODERN_LEVELDB_STRESS_SOURCE_REVISION "\t" +
                    std::string(MODERN_LEVELDB_STRESS_SOURCE_DIRTY != 0 ? "dirty\n"
                                                                       : "clean\n");
                return Write(stdout, identity) ? 0 : 1;
            }
            case CrashStressWorkerMode::WriteHold: {
                Database database =
                    OpenCrashStressDatabase(parsed.database, parsed.compression,
                                            parsed.index == 0);
                ApplyCrashStressBatch(database, parsed.seed, parsed.index);
                if (!Write(stdout, CrashStressAcknowledgement(parsed.index))) {
                    return 1;
                }
                Hold(database);
            }
            case CrashStressWorkerMode::InflightHold: {
                if (!Write(stdout, CrashStressReadiness(parsed.index))) {
                    return 1;
                }
                Database database =
                    OpenCrashStressDatabase(parsed.database, parsed.compression, true);
                ApplyCrashStressBatch(database, parsed.seed, parsed.index);
                Hold(database);
            }
            case CrashStressWorkerMode::VerifyPrefix: {
                const std::size_t acknowledged =
                    VerifyCrashStressPrefix(parsed.database, parsed.journal, parsed.seed,
                                            parsed.compression);
                const std::string verified = "verified\t" + std::to_string(acknowledged) + "\n";
                return Write(stdout, verified) ? 0 : 1;
            }
            case CrashStressWorkerMode::VerifyOptional: {
                const OptionalCrashBatchState state = VerifyOptionalCrashStressBatch(
                    parsed.database, parsed.seed, parsed.index, parsed.compression);
                const std::string verified =
                    state == OptionalCrashBatchState::Absent ? "optional\tabsent\n"
                                                             : "optional\tcomplete\n";
                return Write(stdout, verified) ? 0 : 1;
            }
        }
    } catch (const std::invalid_argument& error) {
        const std::string message =
            std::string(error.what()) + "\n" + CrashStressWorkerUsage();
        static_cast<void>(Write(stderr, message));
        return 2;
    } catch (const std::exception& error) {
        const std::string message = std::string(error.what()) + "\n";
        static_cast<void>(Write(stderr, message));
        return 1;
    }
    return 1;
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
