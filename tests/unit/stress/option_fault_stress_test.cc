#include "stress/option_fault_stress.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <set>
#include <string>
#include <tuple>

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

}  // namespace
}  // namespace modern_leveldb::stress
