#include "stress/mixed_stress.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

#include "stress/stress_support.h"
#include "support/temporary_directory.h"
#include "support/text_file.h"

namespace modern_leveldb::stress {
namespace {

TEST(MixedWorkloadPlanTest, SelectsThePinnedApplicableOperationPercentages) {
    std::array<std::size_t, 5> counts{};
    for (std::uint32_t percentile = 0; percentile < 100; ++percentile) {
        ++counts[static_cast<std::size_t>(SelectMixedOperation(percentile))];
    }

    EXPECT_EQ(counts, (std::array<std::size_t, 5>{10, 20, 45, 15, 10}));
    EXPECT_THROW(static_cast<void>(SelectMixedOperation(100)), std::invalid_argument);
}

TEST(MixedWorkloadPlanTest, PermutesSlotsIntoUniqueBinaryKeysAndPrefixes) {
    EXPECT_EQ(StressHex(MixedKey(0)), "e220a8397b1dcdaf");
    std::set<std::string> keys;
    std::set<std::string> prefixes;
    for (std::uint64_t slot = 0; slot < 4096; ++slot) {
        const std::string key = MixedKey(slot);
        ASSERT_EQ(key.size(), 8U);
        EXPECT_TRUE(keys.insert(key).second);
        const std::string_view prefix = MixedKeyPrefix(key);
        ASSERT_EQ(prefix.size(), 2U);
        prefixes.emplace(prefix);
    }
    EXPECT_GT(prefixes.size(), 4000U);
    EXPECT_THROW(static_cast<void>(MixedKeyPrefix("short")), std::invalid_argument);
}

TEST(MixedWorkloadPlanTest, GeneratesDeterministicVariableLengthBinaryValues) {
    const std::string first = MixedValue(301, 7, 11, 0);
    const std::string second = MixedValue(301, 7, 11, 1);
    const std::string third = MixedValue(301, 7, 11, 2);

    EXPECT_EQ(first.size(), 8U);
    EXPECT_EQ(second.size(), 16U);
    EXPECT_EQ(third.size(), 24U);
    EXPECT_EQ(first, MixedValue(301, 7, 11, 0));
    EXPECT_NE(first, MixedValue(302, 7, 11, 0));
    EXPECT_NE(second, MixedValue(301, 8, 11, 1));
    EXPECT_NE(third, MixedValue(301, 7, 12, 2));
}

TEST(MixedWorkloadPlanTest, DistributesOperationsAcrossReopenEpochs) {
    EXPECT_EQ(MixedEpochOperationCounts(10, 2), (std::vector<std::size_t>{4, 3, 3}));
    EXPECT_EQ(MixedEpochOperationCounts(11, 0), (std::vector<std::size_t>{11}));
    EXPECT_EQ(MixedEpochOperationCounts(11, 10), (std::vector<std::size_t>(11, 1)));
    EXPECT_THROW(static_cast<void>(MixedEpochOperationCounts(0, 0)), std::invalid_argument);
    EXPECT_THROW(static_cast<void>(MixedEpochOperationCounts(10, 10)), std::invalid_argument);
}

MixedStressArguments SmallMixedRun(const std::filesystem::path& root, std::string_view name) {
    MixedStressArguments result;
    result.database = root / (std::string(name) + "-db");
    result.output = root / (std::string(name) + "-output");
    result.seed = 17;
    result.threads = 4;
    result.operations_per_thread = 100;
    result.keys = 256;
    result.reopens = 2;
    result.compression = Compression::None;
    return result;
}

class ThrowBeforeEpoch final : public MixedStressObserver {
public:
    explicit ThrowBeforeEpoch(std::size_t selected) : selected_(selected) {}

    void BeforeEpoch(std::size_t epoch) override {
        if (epoch == selected_) {
            throw std::runtime_error("injected mixed stress failure");
        }
    }

private:
    std::size_t selected_;
};

TEST(MixedStressArgumentsTest, ParsesRocksDbAlignedDefaultsAndOverrides) {
    const std::vector<std::filesystem::path> defaults{
        "--db", "db", "--output", "out",
    };
    const MixedStressArguments parsed_defaults = ParseMixedStressArguments(defaults);
    EXPECT_EQ(parsed_defaults.seed, 301U);
    EXPECT_EQ(parsed_defaults.threads, 32U);
    EXPECT_EQ(parsed_defaults.operations_per_thread, 1'200'000U);
    EXPECT_EQ(parsed_defaults.keys, 1'048'576U);
    EXPECT_EQ(parsed_defaults.reopens, 10U);
    EXPECT_EQ(parsed_defaults.compression, Compression::Snappy);

    const std::vector<std::filesystem::path> overrides{
        "--db",          "db", "--output",              "out",
        "--seed",        "18446744073709551615",
        "--threads",     "256",
        "--operations-per-thread", "42",
        "--keys",        "1024",
        "--reopens",     "3",
        "--compression", "zstd",
    };
    const MixedStressArguments parsed_overrides = ParseMixedStressArguments(overrides);
    EXPECT_EQ(parsed_overrides.seed, std::numeric_limits<std::uint64_t>::max());
    EXPECT_EQ(parsed_overrides.threads, 256U);
    EXPECT_EQ(parsed_overrides.operations_per_thread, 42U);
    EXPECT_EQ(parsed_overrides.keys, 1024U);
    EXPECT_EQ(parsed_overrides.reopens, 3U);
    EXPECT_EQ(parsed_overrides.compression, Compression::Zstd);
}

TEST(MixedStressArgumentsTest, RejectsInvalidAndDuplicateArguments) {
    const std::vector<std::vector<std::filesystem::path>> invalid{
        {},
        {"--db", "db", "--output", "out", "--threads", "0"},
        {"--db", "db", "--output", "out", "--operations-per-thread", "0"},
        {"--db", "db", "--output", "out", "--keys", "0"},
        {"--db", "db", "--output", "out", "--operations-per-thread", "10", "--reopens", "10"},
        {"--db", "db", "--output", "out", "--threads", "4", "--threads", "5"},
        {"--db", "db", "--output", "out", "--keys", "10", "--keys", "11"},
        {"--db", "db", "--output", "out", "--compression", "brotli"},
        {"--db", "db", "--output", "out", "--unknown", "1"},
    };
    for (const auto& arguments : invalid) {
        EXPECT_THROW(static_cast<void>(ParseMixedStressArguments(arguments)),
                     std::invalid_argument);
    }

    const std::array<std::filesystem::path, 1> help{"--help"};
    EXPECT_TRUE(ParseMixedStressArguments(help).help);
}

TEST(MixedStressRunTest, ExecutesApplicableMixAcrossReopenEpochs) {
    test_support::TemporaryDirectory root;
    const StressBuildIdentity identity{"test-revision", false};
    const MixedStressArguments arguments = SmallMixedRun(root.path(), "complete");

    const MixedStressSummary summary = RunMixedStress(arguments, identity);

    EXPECT_EQ(summary.threads, 4U);
    EXPECT_EQ(summary.operations_per_thread, 100U);
    EXPECT_EQ(summary.reopens, 2U);
    EXPECT_EQ(summary.reads + summary.prefixes + summary.writes + summary.deletes +
                  summary.iterations,
              400U);
    EXPECT_GT(summary.reads, 0U);
    EXPECT_GT(summary.prefixes, 0U);
    EXPECT_GT(summary.writes, 0U);
    EXPECT_GT(summary.deletes, 0U);
    EXPECT_GT(summary.iterations, 0U);
    EXPECT_GT(summary.fresh_writes, 0U);
    EXPECT_LE(summary.fresh_writes, summary.writes);
    EXPECT_EQ(summary.exact_checks, 5U);
    EXPECT_LE(summary.entries, arguments.keys);
    EXPECT_EQ(summary.fingerprint.size(), 16U);
    EXPECT_TRUE(std::filesystem::exists(arguments.output / "run.txt"));
    EXPECT_TRUE(std::filesystem::exists(arguments.output / "operation-summary.txt"));
    EXPECT_TRUE(std::filesystem::exists(arguments.output / "epoch-summary.tsv"));
    EXPECT_TRUE(std::filesystem::exists(arguments.output / "final-state.txt"));
    EXPECT_TRUE(std::filesystem::exists(arguments.output / "completion.txt"));
    EXPECT_FALSE(std::filesystem::exists(arguments.output / "failure.txt"));
}

TEST(MixedStressRunTest, SameSeedPreservesOperationCountsAcrossSchedules) {
    test_support::TemporaryDirectory root;
    const StressBuildIdentity identity{"test-revision", false};
    const MixedStressArguments first = SmallMixedRun(root.path(), "first");
    const MixedStressArguments second = SmallMixedRun(root.path(), "second");

    const MixedStressSummary first_summary = RunMixedStress(first, identity);
    const MixedStressSummary second_summary = RunMixedStress(second, identity);

    EXPECT_EQ(first_summary.reads, second_summary.reads);
    EXPECT_EQ(first_summary.prefixes, second_summary.prefixes);
    EXPECT_EQ(first_summary.writes, second_summary.writes);
    EXPECT_EQ(first_summary.deletes, second_summary.deletes);
    EXPECT_EQ(first_summary.iterations, second_summary.iterations);
    EXPECT_EQ(first_summary.fresh_writes, second_summary.fresh_writes);
}

TEST(MixedStressRunTest, PreservesCompletedEpochsOnFailure) {
    test_support::TemporaryDirectory root;
    const StressBuildIdentity identity{"test-revision", false};
    const MixedStressArguments arguments = SmallMixedRun(root.path(), "failure");
    ThrowBeforeEpoch observer(1);

    EXPECT_THROW(static_cast<void>(RunMixedStress(arguments, identity, &observer)),
                 std::runtime_error);

    const std::string epochs =
        test_support::ReadTextFile(arguments.output / "epoch-summary.tsv");
    EXPECT_NE(epochs.find("\n0\t"), std::string::npos);
    EXPECT_EQ(epochs.find("\n1\t"), std::string::npos);
    EXPECT_NE(test_support::ReadTextFile(arguments.output / "failure.txt")
                  .find("injected mixed stress failure"),
              std::string::npos);
    EXPECT_FALSE(std::filesystem::exists(arguments.output / "completion.txt"));
    EXPECT_FALSE(std::filesystem::exists(arguments.output / "final-state.txt"));
}

TEST(MixedStressRunTest, RejectsUnsafePathsBeforeStartingThreads) {
    test_support::TemporaryDirectory root;
    const StressBuildIdentity identity{"test-revision", false};
    MixedStressArguments arguments = SmallMixedRun(root.path(), "existing");
    std::filesystem::create_directory(arguments.database);

    EXPECT_THROW(static_cast<void>(RunMixedStress(arguments, identity)), std::invalid_argument);
    EXPECT_FALSE(std::filesystem::exists(arguments.output));
}

TEST(MixedStressRunTest, UsageStatesPinnedProfileAndLocalBoundary) {
    const std::string usage = MixedStressUsage();

    EXPECT_NE(usage.find("1200000"), std::string::npos);
    EXPECT_NE(usage.find("1048576"), std::string::npos);
    EXPECT_NE(usage.find("10/20/45/15/10"), std::string::npos);
    EXPECT_NE(usage.find("disposable"), std::string::npos);
}

}  // namespace
}  // namespace modern_leveldb::stress
