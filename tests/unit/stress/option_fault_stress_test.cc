#include "stress/option_fault_stress.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>

#include "support/temporary_directory.h"
#include "support/text_file.h"

namespace modern_leveldb::stress {
namespace {

TEST(OptionMatrixTest, EnumeratesCompleteCartesianProductWithStableIds) {
    const std::vector<OptionMatrixCell> cells = EnumerateOptionMatrix();

    ASSERT_EQ(cells.size(), 48U);
    std::set<std::string> ids;
    std::set<std::tuple<Compression, bool, std::uint32_t, std::size_t, bool>> tuples;
    for (const OptionMatrixCell& cell : cells) {
        EXPECT_TRUE(ids.insert(OptionMatrixCellId(cell)).second);
        EXPECT_TRUE(tuples
                        .insert({cell.compression, cell.allow_mmap_reads,
                                 cell.bloom_bits_per_key, cell.write_buffer_size,
                                 cell.sync_wal_creation})
                        .second);
    }

    constexpr std::array<Compression, 3> compressions{
        Compression::None,
        Compression::Snappy,
        Compression::Zstd,
    };
    constexpr std::array<bool, 2> booleans{true, false};
    constexpr std::array<std::uint32_t, 2> bloom_bits{0, 10};
    constexpr std::array<std::size_t, 2> write_buffers{64U * 1024U, 256U * 1024U};
    for (const Compression compression : compressions) {
        for (const bool allow_mmap_reads : booleans) {
            for (const std::uint32_t bloom : bloom_bits) {
                for (const std::size_t write_buffer : write_buffers) {
                    for (const bool sync_wal_creation : booleans) {
                        EXPECT_EQ(tuples.count({compression, allow_mmap_reads, bloom,
                                               write_buffer, sync_wal_creation}),
                                  1U);
                    }
                }
            }
        }
    }

    EXPECT_EQ(OptionMatrixCellId(cells.front()),
              "c-none_access-map_bloom-0_wb-65536_wal-create-sync");
    EXPECT_EQ(OptionMatrixCellId(cells.back()),
              "c-zstd_access-copy_bloom-10_wb-262144_wal-create-async");
}

TEST(OptionMatrixTest, DerivesStableCellSpecificSeeds) {
    const std::vector<OptionMatrixCell> cells = EnumerateOptionMatrix();

    EXPECT_EQ(DeriveOptionMatrixSeed(301, cells.front()), 592983571980980358ULL);
    EXPECT_EQ(DeriveOptionMatrixSeed(301, cells.front()),
              DeriveOptionMatrixSeed(301, cells.front()));
    EXPECT_NE(DeriveOptionMatrixSeed(301, cells.front()),
              DeriveOptionMatrixSeed(301, cells.back()));
    EXPECT_NE(DeriveOptionMatrixSeed(301, cells.front()),
              DeriveOptionMatrixSeed(302, cells.front()));
}

TEST(OptionMatrixTest, MapsEveryCellFieldToPublicOptions) {
    const OptionMatrixCell cell{
        Compression::Zstd,
        false,
        10,
        256U * 1024U,
        false,
    };

    const Options options = OptionMatrixDatabaseOptions(cell);

    EXPECT_TRUE(options.create_if_missing);
    EXPECT_TRUE(options.error_if_exists);
    EXPECT_EQ(options.compression, Compression::Zstd);
    EXPECT_FALSE(options.allow_mmap_reads);
    EXPECT_EQ(options.bloom_bits_per_key, 10U);
    EXPECT_EQ(options.write_buffer_size, 256U * 1024U);
    EXPECT_FALSE(options.sync_wal_creation);
#if defined(_WIN32)
    EXPECT_TRUE(options.allow_weak_namespace_durability);
#endif
}

TEST(OptionMatrixTest, FingerprintsSortedBinaryStateWithStableFormat) {
    test_support::Model model;
    model.emplace("a", "1");
    model.emplace(std::string("b\0", 2), std::string("\xff", 1));

    EXPECT_EQ(StableModelFingerprint({}), "cbf29ce484222325");
    EXPECT_EQ(StableModelFingerprint(model), "c1900cb37fcee501");
}

TEST(OptionMatrixCellTest, ReopensAndProducesDeterministicSemanticSummary) {
    test_support::TemporaryDirectory root;
    const OptionMatrixCell cell{
        Compression::None,
        false,
        0,
        64U * 1024U,
        false,
    };

    const OptionMatrixCellSummary first =
        RunOptionMatrixCell(root.path() / "first", cell, 17, 96);
    const OptionMatrixCellSummary second =
        RunOptionMatrixCell(root.path() / "second", cell, 17, 96);

    EXPECT_EQ(first, second);
    EXPECT_EQ(first.id, OptionMatrixCellId(cell));
    EXPECT_EQ(first.seed, DeriveOptionMatrixSeed(17, cell));
    EXPECT_EQ(first.operations, 96U);
    EXPECT_EQ(first.fingerprint.size(), 16U);
    EXPECT_TRUE(std::filesystem::exists(root.path() / "first" / "CURRENT"));
    EXPECT_TRUE(std::filesystem::exists(root.path() / "second" / "CURRENT"));
}

TEST(FaultScenarioTest, EnumeratesStableNamedBoundaries) {
    const std::vector<FaultScenario> scenarios = EnumerateFaultScenarios();

    EXPECT_EQ(scenarios,
              (std::vector<FaultScenario>{
                  FaultScenario::Read,
                  FaultScenario::WalAppend,
                  FaultScenario::WalSync,
                  FaultScenario::OldWalClose,
                  FaultScenario::TableSync,
                  FaultScenario::TableClose,
                  FaultScenario::TableDirectorySync,
                  FaultScenario::ManifestAppend,
                  FaultScenario::ManifestSync,
                  FaultScenario::Scheduling,
              }));
    EXPECT_EQ(FaultScenarioName(FaultScenario::Read), "read");
    EXPECT_EQ(FaultScenarioName(FaultScenario::WalAppend), "wal-append");
    EXPECT_EQ(FaultScenarioName(FaultScenario::WalSync), "wal-sync");
    EXPECT_EQ(FaultScenarioName(FaultScenario::OldWalClose), "old-wal-close");
    EXPECT_EQ(FaultScenarioName(FaultScenario::TableSync), "table-sync");
    EXPECT_EQ(FaultScenarioName(FaultScenario::TableClose), "table-close");
    EXPECT_EQ(FaultScenarioName(FaultScenario::TableDirectorySync),
              "table-directory-sync");
    EXPECT_EQ(FaultScenarioName(FaultScenario::ManifestAppend), "manifest-append");
    EXPECT_EQ(FaultScenarioName(FaultScenario::ManifestSync), "manifest-sync");
    EXPECT_EQ(FaultScenarioName(FaultScenario::Scheduling), "scheduling");
}

class FaultScenarioContractTest : public testing::TestWithParam<FaultScenario> {};

TEST_P(FaultScenarioContractTest, EnforcesRetryStickyAndWholeBatchOracles) {
    const FaultScenario scenario = GetParam();
    const FaultScenarioSummary summary = RunFaultScenario(scenario);

    EXPECT_EQ(summary.scenario, scenario);
    EXPECT_EQ(summary.name, FaultScenarioName(scenario));
    EXPECT_FALSE(summary.injected_operation.empty());
    EXPECT_EQ(summary.error_message,
              "injected " + FaultScenarioName(scenario) + " failure");
    if (scenario == FaultScenario::Read) {
        EXPECT_EQ(summary.error_code, ErrorCode::Io);
        EXPECT_FALSE(summary.sticky);
        EXPECT_FALSE(summary.later_write_reused_error_without_io);
        EXPECT_TRUE(summary.retry_succeeded);
        EXPECT_FALSE(summary.recovered_batch.has_value());
        return;
    }

    EXPECT_EQ(summary.error_code,
              scenario == FaultScenario::Scheduling ? ErrorCode::Busy
                                                    : ErrorCode::Io);
    EXPECT_TRUE(summary.sticky);
    EXPECT_TRUE(summary.later_write_reused_error_without_io);
    EXPECT_FALSE(summary.retry_succeeded);
    ASSERT_TRUE(summary.recovered_batch.has_value());
    if (scenario == FaultScenario::WalAppend ||
        scenario == FaultScenario::OldWalClose ||
        scenario == FaultScenario::Scheduling) {
        EXPECT_EQ(*summary.recovered_batch, RecoveredBatchState::Old);
    } else {
        EXPECT_EQ(*summary.recovered_batch, RecoveredBatchState::New);
    }
}

INSTANTIATE_TEST_SUITE_P(
    NamedBoundary, FaultScenarioContractTest,
    testing::ValuesIn(EnumerateFaultScenarios()),
    [](const testing::TestParamInfo<FaultScenario>& information) {
        std::string name = FaultScenarioName(information.param);
        for (char& character : name) {
            if (character == '-') {
                character = '_';
            }
        }
        return name;
    });

TEST(LivenessScenarioTest, DrainsWorkAndVerifiesFinalAndReopenedModel) {
    const LivenessScenarioSummary first = RunLivenessScenario(17, 256);
    const LivenessScenarioSummary second = RunLivenessScenario(17, 256);

    EXPECT_EQ(first, second);
    EXPECT_EQ(first.seed, 17U);
    EXPECT_EQ(first.writes, 256U);
    EXPECT_GT(first.background_tasks, 0U);
    EXPECT_GT(first.table_outputs, 0U);
    EXPECT_GT(first.point_reads, 0U);
    EXPECT_GT(first.scans, 0U);
    EXPECT_EQ(first.entries, first.writes);
    EXPECT_EQ(first.fingerprint.size(), 16U);
    EXPECT_THROW(static_cast<void>(RunLivenessScenario(17, 0)),
                 std::invalid_argument);
}

OptionFaultStressArguments SmallOptionFaultRun(
    const std::filesystem::path& root, std::string_view name) {
    OptionFaultStressArguments result;
    result.workspace = root / std::string(name);
    result.seed = 17;
    result.operations = 32;
    result.liveness_writes = 64;
    return result;
}

class ThrowBeforeFault final : public OptionFaultStressObserver {
public:
    void BeforeOptionCell(std::size_t, const OptionMatrixCell&) override {}
    void BeforeFaultScenario(std::size_t, FaultScenario) override {
        throw std::runtime_error("injected option-fault observer failure");
    }
    void BeforeLiveness() override {}
};

TEST(OptionFaultStressArgumentsTest, ParsesDefaultsOverridesAndNativePaths) {
#if defined(_WIN32)
    const std::filesystem::path workspace = L"matrix-\u6570\u636e-\U0001f680";
#else
    const std::filesystem::path workspace =
        "matrix-\xe6\x95\xb0\xe6\x8d\xae-\xf0\x9f\x9a\x80";
#endif
    const std::array<std::filesystem::path, 2> defaults{
        "--workspace",
        workspace,
    };
    const OptionFaultStressArguments parsed_defaults =
        ParseOptionFaultStressArguments(defaults);
    EXPECT_EQ(parsed_defaults.workspace, workspace);
    EXPECT_EQ(parsed_defaults.seed, 301U);
    EXPECT_EQ(parsed_defaults.operations, 250U);
    EXPECT_EQ(parsed_defaults.liveness_writes, 2'000U);

    const std::vector<std::filesystem::path> overrides{
        "--workspace", workspace,
        "--seed", "18446744073709551615",
        "--operations", "42",
        "--liveness-writes", "84",
    };
    const OptionFaultStressArguments parsed_overrides =
        ParseOptionFaultStressArguments(overrides);
    EXPECT_EQ(parsed_overrides.seed,
              std::numeric_limits<std::uint64_t>::max());
    EXPECT_EQ(parsed_overrides.operations, 42U);
    EXPECT_EQ(parsed_overrides.liveness_writes, 84U);
}

TEST(OptionFaultStressArgumentsTest, RejectsMissingDuplicateAndInvalidOptions) {
    const std::vector<std::vector<std::filesystem::path>> invalid{
        {},
        {"--workspace"},
        {"--workspace", ""},
        {"--workspace", "work", "--workspace", "other"},
        {"--workspace", "work", "--seed"},
        {"--workspace", "work", "--seed", "x"},
        {"--workspace", "work", "--operations", "0"},
        {"--workspace", "work", "--operations", "18446744073709551616"},
        {"--workspace", "work", "--liveness-writes", "0"},
        {"--workspace", "work", "--operations", "1", "--operations", "2"},
        {"--workspace", "work", "--liveness-writes", "1",
         "--liveness-writes", "2"},
        {"--workspace", "work", "--unknown", "1"},
    };
    for (const auto& arguments : invalid) {
        EXPECT_THROW(
            static_cast<void>(ParseOptionFaultStressArguments(arguments)),
            std::invalid_argument);
    }

    const std::array<std::filesystem::path, 1> help{"--help"};
    EXPECT_TRUE(ParseOptionFaultStressArguments(help).help);
}

TEST(OptionFaultStressRunTest, PublishesDeterministicVersionedArtifacts) {
    test_support::TemporaryDirectory root;
    const StressBuildIdentity identity{"test-revision", false};
    const std::array<OptionMatrixCell, 1> cells{EnumerateOptionMatrix().front()};
    const std::array<FaultScenario, 1> faults{FaultScenario::Read};
    OptionFaultStressArguments first =
        SmallOptionFaultRun(root.path(), "first");
#if defined(_WIN32)
    first.workspace = root.path() / L"first-\u6570\u636e-\U0001f680";
#else
    first.workspace =
        root.path() / "first-\xe6\x95\xb0\xe6\x8d\xae-\xf0\x9f\x9a\x80";
#endif
    const OptionFaultStressArguments second =
        SmallOptionFaultRun(root.path(), "second");

    const OptionFaultStressSummary first_summary =
        RunOptionFaultStress(first, identity, cells, faults);
    const OptionFaultStressSummary second_summary =
        RunOptionFaultStress(second, identity, cells, faults);

    EXPECT_EQ(first_summary, second_summary);
    EXPECT_EQ(first_summary.option_cells, 1U);
    EXPECT_EQ(first_summary.fault_scenarios, 1U);
    EXPECT_EQ(first_summary.operations_per_cell, first.operations);
    EXPECT_EQ(first_summary.liveness.writes, first.liveness_writes);
    const std::array<std::filesystem::path, 5> artifacts{
        "run.txt",
        "cells.tsv",
        "faults.tsv",
        "liveness.txt",
        "completion.txt",
    };
    for (const std::filesystem::path& artifact : artifacts) {
        EXPECT_EQ(test_support::ReadTextFile(first.workspace / "artifacts" /
                                             artifact),
                  test_support::ReadTextFile(second.workspace / "artifacts" /
                                             artifact));
    }
    const std::string run =
        test_support::ReadTextFile(first.workspace / "artifacts" / "run.txt");
    EXPECT_NE(run.find("revision=test-revision"), std::string::npos);
    EXPECT_NE(run.find("option_cells=1"), std::string::npos);
    EXPECT_NE(run.find("fault_scenarios=1"), std::string::npos);
    EXPECT_TRUE(std::filesystem::exists(
        first.workspace / "databases" / OptionMatrixCellId(cells.front()) /
        "CURRENT"));
    EXPECT_FALSE(std::filesystem::exists(first.workspace / "artifacts" /
                                         "failure.txt"));
}

TEST(OptionFaultStressRunTest, RejectsUnsafeWorkspaceWithoutMutation) {
    test_support::TemporaryDirectory root;
    const StressBuildIdentity identity{"test-revision", false};
    const std::array<OptionMatrixCell, 1> cells{EnumerateOptionMatrix().front()};
    const std::array<FaultScenario, 1> faults{FaultScenario::Read};

    OptionFaultStressArguments existing =
        SmallOptionFaultRun(root.path(), "existing");
    std::filesystem::create_directory(existing.workspace);
    WriteStressText(existing.workspace / "sentinel", "keep");
    EXPECT_THROW(
        static_cast<void>(
            RunOptionFaultStress(existing, identity, cells, faults)),
        std::invalid_argument);
    EXPECT_EQ(test_support::ReadTextFile(existing.workspace / "sentinel"),
              "keep");

    OptionFaultStressArguments missing_parent =
        SmallOptionFaultRun(root.path(), "missing");
    missing_parent.workspace =
        root.path() / "absent-parent" / "workspace";
    EXPECT_THROW(
        static_cast<void>(
            RunOptionFaultStress(missing_parent, identity, cells, faults)),
        std::invalid_argument);
    EXPECT_FALSE(std::filesystem::exists(missing_parent.workspace));
}

TEST(OptionFaultStressRunTest, PreservesFailureAndWithholdsCompletion) {
    test_support::TemporaryDirectory root;
    const StressBuildIdentity identity{"test-revision", true};
    const std::array<OptionMatrixCell, 1> cells{EnumerateOptionMatrix().front()};
    const std::array<FaultScenario, 1> faults{FaultScenario::Read};
    const OptionFaultStressArguments arguments =
        SmallOptionFaultRun(root.path(), "failure");
    ThrowBeforeFault observer;

    EXPECT_THROW(
        static_cast<void>(
            RunOptionFaultStress(arguments, identity, cells, faults, &observer)),
        std::runtime_error);

    const std::filesystem::path artifacts = arguments.workspace / "artifacts";
    EXPECT_TRUE(std::filesystem::exists(artifacts / "cells.tsv"));
    EXPECT_TRUE(std::filesystem::exists(artifacts / "faults.tsv"));
    EXPECT_NE(test_support::ReadTextFile(artifacts / "failure.txt")
                  .find("injected option-fault observer failure"),
              std::string::npos);
    EXPECT_FALSE(std::filesystem::exists(artifacts / "liveness.txt"));
    EXPECT_FALSE(std::filesystem::exists(artifacts / "completion.txt"));
}

TEST(OptionFaultStressRunTest, UsageStatesLocalOnlySafetyBoundary) {
    const std::string usage = OptionFaultStressUsage();

    EXPECT_NE(usage.find("local"), std::string::npos);
    EXPECT_NE(usage.find("disposable"), std::string::npos);
    EXPECT_NE(usage.find("--workspace"), std::string::npos);
    EXPECT_NE(usage.find("--liveness-writes"), std::string::npos);
}

}  // namespace
}  // namespace modern_leveldb::stress
