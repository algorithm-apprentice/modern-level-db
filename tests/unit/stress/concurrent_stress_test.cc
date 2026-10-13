#include "stress/concurrent_stress.h"

#include <gtest/gtest.h>

#include <array>
#include <filesystem>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

#include "support/temporary_directory.h"
#include "support/text_file.h"

namespace modern_leveldb::stress {
namespace {

std::size_t Count(std::string_view text, std::string_view pattern) {
    std::size_t count = 0;
    for (std::size_t offset = text.find(pattern); offset != std::string_view::npos;
         offset = text.find(pattern, offset + pattern.size())) {
        ++count;
    }
    return count;
}

ConcurrentStressArguments SmallConcurrentRun(const std::filesystem::path& root,
                                             std::string_view name) {
    ConcurrentStressArguments result;
    result.database = root / (std::string(name) + "-db");
    result.output = root / (std::string(name) + "-output");
    result.seed = 17;
    result.mode = ConcurrentStressMode::Checked;
    result.threads = 4;
    result.rounds = 24;
    result.compression = Compression::None;
    return result;
}

class ThrowAtRound final : public ConcurrentStressObserver {
public:
    explicit ThrowAtRound(std::size_t selected) : selected_(selected) {}

    void BeforeLinearizabilityRound(std::size_t round) override {
        if (round == selected_) {
            throw std::runtime_error("injected concurrent stress failure");
        }
    }

private:
    std::size_t selected_;
};

TEST(ConcurrentStressArgumentsTest, ParsesDefaultsOverridesAndBounds) {
    const std::vector<std::filesystem::path> defaults{
        "--db",
        "db",
        "--output",
        "out",
    };
    const ConcurrentStressArguments parsed_defaults = ParseConcurrentStressArguments(defaults);
    EXPECT_EQ(parsed_defaults.seed, 301U);
    EXPECT_EQ(parsed_defaults.mode, ConcurrentStressMode::Fanout);
    EXPECT_EQ(parsed_defaults.threads, 32U);
    EXPECT_EQ(parsed_defaults.rounds, 1000U);
    EXPECT_EQ(parsed_defaults.compression, Compression::None);

    const std::vector<std::filesystem::path> overrides{
        "--db",      "db",  "--output", "out", "--seed",        "18446744073709551615",
        "--threads", "8",   "--rounds", "42",  "--compression", "snappy",
        "--mode",    "checked",
    };
    const ConcurrentStressArguments parsed_overrides = ParseConcurrentStressArguments(overrides);
    EXPECT_EQ(parsed_overrides.seed, std::numeric_limits<std::uint64_t>::max());
    EXPECT_EQ(parsed_overrides.mode, ConcurrentStressMode::Checked);
    EXPECT_EQ(parsed_overrides.threads, 8U);
    EXPECT_EQ(parsed_overrides.rounds, 42U);
    EXPECT_EQ(parsed_overrides.compression, Compression::Snappy);

    const std::vector<std::vector<std::filesystem::path>> invalid{
        {},
        {"--db", "db", "--output", "out", "--mode", "fanout", "--threads", "0"},
        {"--db", "db", "--output", "out", "--mode", "checked", "--threads", "1"},
        {"--db", "db", "--output", "out", "--mode", "checked", "--threads", "9"},
        {"--db", "db", "--output", "out", "--mode", "unknown"},
        {"--db", "db", "--output", "out", "--mode", "checked", "--mode", "fanout"},
        {"--db", "db", "--output", "out", "--rounds", "0"},
        {"--db", "db", "--output", "out", "--threads", "4", "--threads", "5"},
        {"--db", "db", "--output", "out", "--rounds", "1", "--rounds", "2"},
        {"--db", "db", "--output", "out", "--compression", "brotli"},
    };
    for (const auto& arguments : invalid) {
        EXPECT_THROW(static_cast<void>(ParseConcurrentStressArguments(arguments)),
                     std::invalid_argument);
    }

    const std::array<std::filesystem::path, 1> help_arguments{"--help"};
    EXPECT_TRUE(ParseConcurrentStressArguments(help_arguments).help);
}

TEST(ConcurrentStressArgumentsTest, FanoutAcceptsAnyPositiveRepresentableThreadCount) {
    for (const std::string_view threads : {"1", "32", "256", "1024"}) {
        const std::vector<std::filesystem::path> arguments{
            "--db", "db", "--output", "out", "--threads", threads, "--mode", "fanout",
        };

        const ConcurrentStressArguments parsed = ParseConcurrentStressArguments(arguments);

        EXPECT_EQ(parsed.mode, ConcurrentStressMode::Fanout);
        EXPECT_EQ(parsed.threads,
                  static_cast<std::size_t>(std::stoull(std::string(threads))));
    }
}

TEST(ConcurrentStressRunTest, CompletesLinearizableAndSnapshotPhases) {
    test_support::TemporaryDirectory root;
    const StressBuildIdentity identity{"test-revision", false};
    const ConcurrentStressArguments arguments = SmallConcurrentRun(root.path(), "complete");

    const ConcurrentStressSummary summary = RunConcurrentStress(arguments, identity);

    EXPECT_EQ(summary.rounds, arguments.rounds);
    EXPECT_EQ(summary.threads, arguments.threads);
    EXPECT_EQ(summary.mode, ConcurrentStressMode::Checked);
    EXPECT_EQ(summary.linearized_worker_operations, arguments.rounds * arguments.threads);
    EXPECT_GT(summary.batch_writes, 0U);
    EXPECT_GT(summary.snapshot_checks, 0U);
    const std::string histories =
        test_support::ReadTextFile(arguments.output / "linearizability.log");
    EXPECT_EQ(Count(histories, "\nround="), arguments.rounds);
    EXPECT_NE(histories.find("\tsource="), std::string::npos);
    EXPECT_NE(histories.find("\tyields_before="), std::string::npos);
    EXPECT_NE(histories.find("\tsource=final-read"), std::string::npos);
    const std::string snapshots =
        test_support::ReadTextFile(arguments.output / "snapshot-summary.txt");
    EXPECT_NE(snapshots.find("batch_writes="), std::string::npos);
    EXPECT_NE(snapshots.find("snapshot_checks="), std::string::npos);
    EXPECT_TRUE(std::filesystem::exists(arguments.output / "final-state.txt"));
    EXPECT_TRUE(std::filesystem::exists(arguments.output / "completion.txt"));
    EXPECT_FALSE(std::filesystem::exists(arguments.output / "failure.txt"));
}

TEST(ConcurrentStressRunTest, FanoutRunsOnlyScalableSnapshotAndReopenOracle) {
    test_support::TemporaryDirectory root;
    const StressBuildIdentity identity{"test-revision", false};
    ConcurrentStressArguments arguments = SmallConcurrentRun(root.path(), "fanout");
    arguments.mode = ConcurrentStressMode::Fanout;
    arguments.threads = 32;
    arguments.rounds = 2;

    const ConcurrentStressSummary summary = RunConcurrentStress(arguments, identity);

    EXPECT_EQ(summary.mode, ConcurrentStressMode::Fanout);
    EXPECT_EQ(summary.threads, 32U);
    EXPECT_EQ(summary.rounds, 2U);
    EXPECT_EQ(summary.linearized_worker_operations, 0U);
    EXPECT_EQ(summary.batch_writes, 32U);
    EXPECT_GE(summary.snapshot_checks, 16U);
    EXPECT_EQ(summary.entries, 32U);
    const std::string histories =
        test_support::ReadTextFile(arguments.output / "linearizability.log");
    EXPECT_EQ(Count(histories, "\nround="), 0U);
    EXPECT_NE(test_support::ReadTextFile(arguments.output / "run.txt").find("mode=fanout"),
              std::string::npos);
    EXPECT_NE(test_support::ReadTextFile(arguments.output / "completion.txt").find("mode=fanout"),
              std::string::npos);
}

TEST(ConcurrentStressRunTest, FailurePreservesCompletedHistoriesWithoutCompletion) {
    test_support::TemporaryDirectory root;
    const StressBuildIdentity identity{"test-revision", false};
    const ConcurrentStressArguments arguments = SmallConcurrentRun(root.path(), "failure");
    ThrowAtRound observer(3);

    EXPECT_THROW(static_cast<void>(RunConcurrentStress(arguments, identity, &observer)),
                 std::runtime_error);

    const std::string histories =
        test_support::ReadTextFile(arguments.output / "linearizability.log");
    EXPECT_EQ(Count(histories, "\nround="), 3U);
    EXPECT_NE(test_support::ReadTextFile(arguments.output / "failure.txt")
                  .find("injected concurrent stress failure"),
              std::string::npos);
    EXPECT_FALSE(std::filesystem::exists(arguments.output / "completion.txt"));
    EXPECT_FALSE(std::filesystem::exists(arguments.output / "final-state.txt"));
}

TEST(ConcurrentStressRunTest, SupportsMinimumAndMaximumThreadBounds) {
    test_support::TemporaryDirectory root;
    const StressBuildIdentity identity{"test-revision", false};
    for (const std::size_t threads : {2U, 8U}) {
        ConcurrentStressArguments arguments =
            SmallConcurrentRun(root.path(), "threads-" + std::to_string(threads));
        arguments.threads = threads;
        arguments.rounds = 4;

        const ConcurrentStressSummary summary = RunConcurrentStress(arguments, identity);

        EXPECT_EQ(summary.threads, threads);
        EXPECT_EQ(summary.linearized_worker_operations, threads * arguments.rounds);
        EXPECT_GT(summary.snapshot_checks, 0U);
    }
}

TEST(ConcurrentStressRunTest, RejectsUnsafePathsBeforeStartingThreads) {
    test_support::TemporaryDirectory root;
    const StressBuildIdentity identity{"test-revision", false};
    ConcurrentStressArguments arguments = SmallConcurrentRun(root.path(), "existing");
    std::filesystem::create_directory(arguments.database);

    EXPECT_THROW(static_cast<void>(RunConcurrentStress(arguments, identity)),
                 std::invalid_argument);
    EXPECT_FALSE(std::filesystem::exists(arguments.output));
}

TEST(ConcurrentStressRunTest, UsageStatesBoundedLocalConcurrency) {
    const std::string usage = ConcurrentStressUsage();

    EXPECT_NE(usage.find("2..8"), std::string::npos);
    EXPECT_NE(usage.find("positive"), std::string::npos);
    EXPECT_NE(usage.find("--mode"), std::string::npos);
    EXPECT_NE(usage.find("--rounds"), std::string::npos);
    EXPECT_NE(usage.find("local"), std::string::npos);
}

}  // namespace
}  // namespace modern_leveldb::stress
