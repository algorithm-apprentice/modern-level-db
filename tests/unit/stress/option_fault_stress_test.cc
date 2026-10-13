#include "stress/option_fault_stress.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <set>
#include <string>
#include <tuple>

#include "support/temporary_directory.h"

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
}

}  // namespace
}  // namespace modern_leveldb::stress
