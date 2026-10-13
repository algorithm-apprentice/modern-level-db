#include "stress/mixed_stress.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <set>
#include <stdexcept>
#include <string>

#include "stress/stress_support.h"

namespace modern_leveldb::stress {
namespace {

TEST(MixedWorkloadPlanTest, SelectsThePinnedApplicableOperationPercentages) {
    std::array<std::size_t, 5> counts{};
    for (std::uint32_t percentile = 0; percentile < 100; ++percentile) {
        ++counts[static_cast<std::size_t>(SelectMixedOperation(percentile))];
    }

    EXPECT_EQ(counts, (std::array<std::size_t, 5>{10, 20, 45, 15, 10}));
    EXPECT_THROW(static_cast<void>(SelectMixedOperation(100)),
                 std::invalid_argument);
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
    EXPECT_THROW(static_cast<void>(MixedKeyPrefix("short")),
                 std::invalid_argument);
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
    EXPECT_EQ(MixedEpochOperationCounts(10, 2),
              (std::vector<std::size_t>{4, 3, 3}));
    EXPECT_EQ(MixedEpochOperationCounts(11, 0),
              (std::vector<std::size_t>{11}));
    EXPECT_EQ(MixedEpochOperationCounts(11, 10),
              (std::vector<std::size_t>(11, 1)));
    EXPECT_THROW(static_cast<void>(MixedEpochOperationCounts(0, 0)),
                 std::invalid_argument);
    EXPECT_THROW(static_cast<void>(MixedEpochOperationCounts(10, 10)),
                 std::invalid_argument);
}

}  // namespace
}  // namespace modern_leveldb::stress
