#include "table/full_filter.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "modern_leveldb/base/coding.h"
#include "modern_leveldb/base/hash.h"
#include "table/block_format.h"
#include "table/bloom_filter.h"
#include "table/full_filter_probe.h"

namespace modern_leveldb {
namespace {

// Empty, "a", "alpha", and binary00ff; six probes in one 512-bit region.
constexpr std::array<unsigned char, 80> Golden{
    0x4d, 0x4c, 0x46, 0x31, 0x06, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x01, 0x00, 0x00, 0x20, 0x00, 0x00, 0x00, 0x00, 0x08, 0x00, 0x00, 0x00, 0x08, 0x08, 0x00,
    0x01, 0x00, 0x00, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x44, 0x00, 0x00, 0x04,
    0x09, 0x00, 0x40, 0x00, 0x00, 0x20, 0x00, 0x10, 0x00, 0x28, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x20, 0x50, 0x00, 0x00, 0x00, 0x41, 0x00, 0x00, 0x00,
};

static_assert(!std::is_copy_constructible_v<FullFilterReader>);
static_assert(std::is_nothrow_move_constructible_v<FullFilterReader>);
static_assert(!std::is_move_assignable_v<FullFilterReader>);

std::vector<std::byte> GoldenBytes() {
    const auto bytes = std::as_bytes(std::span(Golden));
    return {bytes.begin(), bytes.end()};
}

std::vector<std::byte> BuildFullFilter(const std::vector<std::string>& keys,
                                      std::uint32_t bits_per_key) {
    FullFilterBuilder builder{BloomFilterPolicy(bits_per_key)};
    for (const auto& key : keys) {
        const auto added = builder.AddKey(AsBytes(key));
        if (!added.has_value()) {
            throw std::runtime_error(added.error().ToString());
        }
    }
    const auto bytes = builder.Finish();
    return {bytes.begin(), bytes.end()};
}

bool ReferenceMayMatch(ByteView key, ByteView filter) {
    const auto regions = DecodeFixed32(filter.subspan(8, 4));
    const auto bucket = Hash32(key, 0xbc9f1d34U);
    const auto region = static_cast<std::uint32_t>((std::uint64_t{bucket} * regions) >> 32U);
    const auto line = filter.subspan(16 + std::size_t{region} * 64, 64);
    auto probe_hash = Hash32(key, 0x9e3779b9U);
    for (unsigned probe = 0; probe < std::to_integer<unsigned>(filter[4]); ++probe) {
        const auto bit = probe_hash >> 23U;
        if ((std::to_integer<unsigned>(line[bit / 8]) & (1U << (bit % 8))) == 0) {
            return false;
        }
        probe_hash *= 0x9e3779b9U;
    }
    return true;
}

TEST(FullFilterTest, PreservesTheLiteralVersionOneRepresentation) {
    const std::vector<std::string> keys{"", "a", "alpha", std::string("\0\xff", 2)};
    const auto bytes = BuildFullFilter(keys, 10);
    EXPECT_EQ(bytes, GoldenBytes());
    auto reader = FullFilterReader::Create(bytes);
    ASSERT_TRUE(reader.has_value());
    for (const auto& key : keys) {
        EXPECT_TRUE(reader->KeyMayMatch(AsBytes(key)));
    }
    EXPECT_EQ(FullFilterMetaKey, "filter.modern_leveldb.FullBloom1");
}

TEST(FullFilterTest, EmptyBuilderKeepsOneEmptyRegion) {
    const auto bytes = BuildFullFilter({}, 10);
    ASSERT_EQ(bytes.size(), 80U);
    EXPECT_EQ(DecodeFixed32(ByteView(bytes).subspan(8, 4)), 1U);
    EXPECT_TRUE(std::ranges::all_of(ByteView(bytes).subspan(16),
                                   [](std::byte value) { return value == std::byte{0}; }));
    auto reader = FullFilterReader::Create(bytes);
    ASSERT_TRUE(reader.has_value());
    EXPECT_FALSE(reader->KeyMayMatch({}));
    EXPECT_FALSE(reader->KeyMayMatch(AsBytes("absent")));
}

TEST(FullFilterTest, AllProbeCountsAndRegionChoicesMatchTheIndependentBitOracle) {
    std::vector<std::string> keys;
    for (std::uint32_t index = 0; index < 400; ++index) {
        keys.push_back(std::string(index % 7, '\0') + std::to_string(index) + '\xff');
    }
    for (std::uint32_t probes = 1; probes <= 30; ++probes) {
        const auto density = (probes * 100 + 68) / 69;
        const auto bytes = BuildFullFilter(keys, density);
        ASSERT_EQ(std::to_integer<std::uint32_t>(bytes[4]), probes);
        auto reader = FullFilterReader::Create(bytes);
        ASSERT_TRUE(reader.has_value());
        for (const auto& key : keys) {
            EXPECT_TRUE(reader->KeyMayMatch(AsBytes(key))) << probes;
        }
        for (std::uint32_t query = 0; query < 512; ++query) {
            const std::string key = "query-" + std::to_string(query);
            EXPECT_EQ(reader->KeyMayMatch(AsBytes(key)), ReferenceMayMatch(AsBytes(key), bytes))
                << probes << " " << query;
        }
    }
}

TEST(FullFilterTest, ZeroDensityAndDuplicateKeysStayCorrect) {
    const std::vector<std::string> keys(100, std::string("\x80\0", 2));
    const auto bytes = BuildFullFilter(keys, 0);
    EXPECT_EQ(bytes.size(), 80U);
    EXPECT_EQ(bytes[4], std::byte{1});
    auto reader = FullFilterReader::Create(bytes);
    ASSERT_TRUE(reader.has_value());
    EXPECT_TRUE(reader->KeyMayMatch(AsBytes(keys.front())));
}

TEST(FullFilterTest, AdmitsPayloadSizesWithoutReadingOrAllocatingThem) {
    EXPECT_TRUE(IsFullFilterBlockSize(80));
    EXPECT_TRUE(IsFullFilterBlockSize(144));
    EXPECT_TRUE(IsFullFilterBlockSize(4294967248ULL));
    for (const auto size : {0ULL, 79ULL, 81ULL, 4294967249ULL,
                            std::numeric_limits<unsigned long long>::max()}) {
        EXPECT_FALSE(IsFullFilterBlockSize(size));
    }
}

TEST(FullFilterTest, RejectsKnownMalformedHeadersInOwnedAndBorrowedContents) {
    std::vector<std::vector<std::byte>> malformed;
    malformed.emplace_back();
    malformed.push_back(std::vector<std::byte>(79));
    malformed.push_back(std::vector<std::byte>(81));
    for (const auto offset : {0U, 5U, 6U, 7U, 12U, 13U, 14U, 15U}) {
        auto bytes = GoldenBytes();
        bytes[offset] ^= std::byte{1};
        malformed.push_back(std::move(bytes));
    }
    for (const auto probes : {0U, 31U, 255U}) {
        auto bytes = GoldenBytes();
        bytes[4] = static_cast<std::byte>(probes);
        malformed.push_back(std::move(bytes));
    }
    for (const auto regions : {0U, 2U, std::numeric_limits<std::uint32_t>::max()}) {
        auto bytes = GoldenBytes();
        EncodeFixed32(MutableByteView(bytes).subspan(8, 4), regions);
        malformed.push_back(std::move(bytes));
    }
    for (const auto& bytes : malformed) {
        auto owned = FullFilterReader::Create(bytes);
        ASSERT_FALSE(owned.has_value());
        EXPECT_EQ(owned.error().code(), ErrorCode::Corruption);
        auto borrowed = FullFilterReader::Create(BlockContents::Borrowed(bytes));
        ASSERT_FALSE(borrowed.has_value());
        EXPECT_EQ(borrowed.error().code(), ErrorCode::Corruption);
    }
}

TEST(FullFilterTest, ReaderMovesKeepEveryBorrowedAlignmentStable) {
    for (std::size_t offset = 0; offset < 32; ++offset) {
        std::vector<std::byte> backing(offset + Golden.size());
        const auto golden = GoldenBytes();
        std::ranges::copy(golden, backing.begin() + static_cast<std::ptrdiff_t>(offset));
        auto admitted = FullFilterReader::Create(
            BlockContents::Borrowed(ByteView(backing).subspan(offset)));
        ASSERT_TRUE(admitted.has_value());
        FullFilterReader moved(std::move(*admitted));
        EXPECT_TRUE(moved.KeyMayMatch(AsBytes("alpha")));
    }
    auto owned = FullFilterReader::Create(GoldenBytes());
    ASSERT_TRUE(owned.has_value());
    FullFilterReader moved(std::move(*owned));
    EXPECT_TRUE(moved.KeyMayMatch({}));
}

TEST(FullFilterTest, BuilderOverflowRejectsTheKeyBeforeChangingHashStorage) {
    FullFilterBuilder builder{BloomFilterPolicy(std::numeric_limits<std::uint32_t>::max())};
    for (std::size_t count = 0; count < 7; ++count) {
        ASSERT_TRUE(builder.AddKey(AsBytes("key")).has_value());
    }
    EXPECT_EQ(builder.key_count(), 7U);
    const auto overflow = builder.AddKey(AsBytes("rejected"));
    ASSERT_FALSE(overflow.has_value());
    EXPECT_EQ(overflow.error().code(), ErrorCode::InvalidArgument);
    EXPECT_EQ(builder.key_count(), 7U);
}

TEST(FullFilterTest, ScalarProbeIncludesBit511WithoutReadingPastTheRegion) {
    std::array<std::byte, 64> region{};
    EXPECT_FALSE(full_filter_detail::ScalarMayMatch(0xffffffffU, 1, region.data()));
    region.back() = std::byte{0x80};
    EXPECT_TRUE(full_filter_detail::ScalarMayMatch(0xffffffffU, 1, region.data()));
    EXPECT_FALSE(full_filter_detail::ScalarMayMatch(0xffffffffU, 2, region.data()));
}

}  // namespace
}  // namespace modern_leveldb
