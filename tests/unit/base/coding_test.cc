#include "modern_leveldb/base/coding.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <span>
#include <string_view>
#include <vector>

#include "modern_leveldb/base/bytes.h"
#include "modern_leveldb/base/result.h"

namespace modern_leveldb {
namespace {

template <typename UInt>
struct FixedGolden {
    UInt value;
    std::array<std::byte, sizeof(UInt)> bytes;
};

template <typename UInt>
void CheckFixedGolden(const FixedGolden<UInt>& golden, auto decode, auto consume) {
    constexpr std::size_t Width = sizeof(UInt);
    constexpr std::size_t PrefixSize = sizeof(std::uint64_t);
    constexpr std::size_t OffsetCount = 32;
    for (std::size_t offset = 0; offset < OffsetCount; ++offset) {
        SCOPED_TRACE(offset);
        alignas(std::uint64_t) std::array<std::byte, PrefixSize + OffsetCount + Width> storage;
        storage.fill(std::byte{0xa5});
        std::byte* const begin = storage.data() + PrefixSize + offset;
        std::ranges::copy(golden.bytes, begin);
        const auto original = storage;

        EXPECT_EQ(decode(std::span<const std::byte, Width>(begin, Width)), golden.value);
        ByteView input(begin, Width + 1);
        const auto consumed = consume(input);
        ASSERT_TRUE(consumed.has_value());
        EXPECT_EQ(*consumed, golden.value);
        EXPECT_EQ(input.data(), begin + Width);
        ASSERT_EQ(input.size(), 1U);
        EXPECT_EQ(input.front(), std::byte{0xa5});
        EXPECT_EQ(storage, original);
    }

    const auto exact = std::make_unique<std::byte[]>(Width);
    std::ranges::copy(golden.bytes, exact.get());
    EXPECT_EQ(decode(std::span<const std::byte, Width>(exact.get(), Width)), golden.value);
    ByteView input(exact.get(), Width);
    const auto consumed = consume(input);
    ASSERT_TRUE(consumed.has_value());
    EXPECT_EQ(*consumed, golden.value);
    EXPECT_EQ(input.data(), exact.get() + Width);
    EXPECT_TRUE(input.empty());
    EXPECT_TRUE(std::ranges::equal(ByteView(exact.get(), Width), golden.bytes));
}

template <typename UInt>
void CheckTruncatedFixed(const FixedGolden<UInt>& golden, auto consume, std::string_view message) {
    for (std::size_t offset = 0; offset < sizeof(std::uint64_t); ++offset) {
        SCOPED_TRACE(offset);
        std::array<std::byte, 2 * sizeof(std::uint64_t)> storage;
        storage.fill(std::byte{0xa5});
        std::byte* const begin = storage.data() + offset;
        std::ranges::copy(golden.bytes, begin);
        const auto original = storage;
        for (std::size_t size = 0; size < sizeof(UInt); ++size) {
            SCOPED_TRACE(size);
            ByteView input(begin, size);
            const auto consumed = consume(input);
            ASSERT_FALSE(consumed.has_value());
            EXPECT_EQ(consumed.error().code(), ErrorCode::Corruption);
            EXPECT_EQ(consumed.error().message(), message);
            EXPECT_EQ(input.data(), begin);
            EXPECT_EQ(input.size(), size);
            EXPECT_EQ(storage, original);
            EXPECT_TRUE(
                std::string_view(consumed.error().location().file_name()).ends_with("coding.cc"));
            EXPECT_NE(
                std::string_view(consumed.error().location().function_name()).find("ConsumeFixed"),
                std::string_view::npos);
        }
    }
}

TEST(CodingTest, LiteralFixed32DecodingPreservesEveryAlignmentAndExactWidth) {
    using Golden = FixedGolden<std::uint32_t>;
    const std::array cases{
        Golden{0U, {std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0}}},
        Golden{0x80U, {std::byte{0x80}, std::byte{0}, std::byte{0}, std::byte{0}}},
        Golden{0x80000000U, {std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0x80}}},
        Golden{0x89abcdefU, {std::byte{0xef}, std::byte{0xcd}, std::byte{0xab}, std::byte{0x89}}},
        Golden{0xffffffffU, {std::byte{0xff}, std::byte{0xff}, std::byte{0xff}, std::byte{0xff}}},
    };
    for (const auto& golden : cases) {
        SCOPED_TRACE(golden.value);
        CheckFixedGolden(golden, DecodeFixed32, ConsumeFixed32);
    }
}

TEST(CodingTest, LiteralFixed64DecodingPreservesEveryAlignmentAndExactWidth) {
    using Golden = FixedGolden<std::uint64_t>;
    const std::array cases{
        Golden{0, {}},
        Golden{0x80ULL, {std::byte{0x80}}},
        Golden{0x8000000000000000ULL,
               {std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0},
                std::byte{0}, std::byte{0x80}}},
        Golden{0xfedcba9876543210ULL,
               {std::byte{0x10}, std::byte{0x32}, std::byte{0x54}, std::byte{0x76}, std::byte{0x98},
                std::byte{0xba}, std::byte{0xdc}, std::byte{0xfe}}},
        Golden{0xffffffffffffffffULL,
               {std::byte{0xff}, std::byte{0xff}, std::byte{0xff}, std::byte{0xff}, std::byte{0xff},
                std::byte{0xff}, std::byte{0xff}, std::byte{0xff}}},
    };
    for (const auto& golden : cases) {
        SCOPED_TRACE(golden.value);
        CheckFixedGolden(golden, DecodeFixed64, ConsumeFixed64);
    }
}

TEST(CodingTest, EveryTruncatedFixed32PreservesCursorBytesAndFailureContext) {
    CheckTruncatedFixed(
        FixedGolden<std::uint32_t>{
            0x89abcdefU, {std::byte{0xef}, std::byte{0xcd}, std::byte{0xab}, std::byte{0x89}}},
        ConsumeFixed32, "truncated fixed32");
}

TEST(CodingTest, EveryTruncatedFixed64PreservesCursorBytesAndFailureContext) {
    CheckTruncatedFixed(
        FixedGolden<std::uint64_t>{
            0xfedcba9876543210ULL,
            {std::byte{0x10}, std::byte{0x32}, std::byte{0x54}, std::byte{0x76}, std::byte{0x98},
             std::byte{0xba}, std::byte{0xdc}, std::byte{0xfe}}},
        ConsumeFixed64, "truncated fixed64");
}

TEST(CodingTest, LiteralVarintEmissionKeepsGuardedBytesAndAdmissionCursor) {
    struct Encoding {
        std::uint32_t value;
        std::vector<std::byte> bytes;
    };
    const std::array encodings{
        Encoding{0, {std::byte{0}}},
        Encoding{127, {std::byte{0x7f}}},
        Encoding{128, {std::byte{0x80}, std::byte{1}}},
        Encoding{300, {std::byte{0xac}, std::byte{2}}},
        Encoding{16384, {std::byte{0x80}, std::byte{0x80}, std::byte{1}}},
        Encoding{
            0xffffffffU,
            {std::byte{0xff}, std::byte{0xff}, std::byte{0xff}, std::byte{0xff}, std::byte{0x0f}}},
    };
    for (const auto& encoding : encodings) {
        SCOPED_TRACE(encoding.value);
        for (std::size_t available = 0; available <= encoding.bytes.size() + 1; ++available) {
            SCOPED_TRACE(available);
            std::array<std::byte, 9> storage;
            storage.fill(std::byte{0xa5});
            auto expected = storage;
            MutableByteView output(storage.data() + 1, available);
            const bool admitted = available >= encoding.bytes.size();
            if (admitted) {
                for (std::size_t index = 0; index < encoding.bytes.size(); ++index) {
                    expected[index + 1] = encoding.bytes[index];
                }
            }

            EXPECT_EQ(EncodeVarint32(output, encoding.value), admitted);
            EXPECT_EQ(storage, expected);
            const std::size_t consumed = admitted ? encoding.bytes.size() : 0;
            EXPECT_EQ(output.data(), storage.data() + 1 + consumed);
            EXPECT_EQ(output.size(), available - consumed);
        }
    }
}

TEST(CodingTest, ScalarAdmissionKeepsTerminalTruncationPriorityAndParentContext) {
    const auto check = [](auto decode, std::size_t maximum_bytes, std::byte excess_terminal) {
        std::vector<std::byte> overflow(maximum_bytes, std::byte{0x80});
        overflow.push_back(std::byte{0});
        for (std::size_t size = 0; size <= overflow.size(); ++size) {
            SCOPED_TRACE(size);
            ByteView input = ByteView(overflow).first(size);
            const auto result = decode(input);
            ASSERT_FALSE(result.has_value());
            EXPECT_EQ(result.error().code(), ErrorCode::Corruption);
            EXPECT_EQ(result.error().message(),
                      size < maximum_bytes ? "truncated varint" : "varint overflow");
            EXPECT_EQ(input.data(), overflow.data());
            EXPECT_EQ(input.size(), size);
            EXPECT_TRUE(
                std::string_view(result.error().location().file_name()).ends_with("coding.cc"));
            EXPECT_NE(
                std::string_view(result.error().location().function_name()).find("ConsumeVarint"),
                std::string_view::npos);
        }

        std::vector<std::byte> terminal{std::byte{0x81}};
        for (std::size_t index = 1; index + 1 < maximum_bytes; ++index) {
            terminal.push_back(std::byte{0x80});
        }
        terminal.push_back(excess_terminal);
        terminal.push_back(std::byte{0x5a});
        ByteView input = terminal;
        const auto result = decode(input);
        ASSERT_TRUE(result.has_value());
        EXPECT_EQ(*result, 1U);
        EXPECT_EQ(input.data(), terminal.data() + maximum_bytes);
        ASSERT_EQ(input.size(), 1U);
        EXPECT_EQ(input.front(), std::byte{0x5a});
    };
    check(ConsumeVarint32, 5, std::byte{0x10});
    check(ConsumeVarint64, 10, std::byte{0x02});
}

TEST(CodingTest, LiteralLengthPrefixesKeepFailurePriorityAndBorrowedPayloads) {
    struct PrefixCase {
        std::vector<std::byte> bytes;
        std::string_view error;
        std::size_t prefix_size;
        std::size_t payload_size;
    };
    const std::array cases{
        PrefixCase{{}, "truncated varint", 0, 0},
        PrefixCase{{std::byte{0x80}}, "truncated varint", 0, 0},
        PrefixCase{{std::byte{0xff}, std::byte{0xff}, std::byte{0xff}, std::byte{0xff},
                    std::byte{0xff}, std::byte{0}},
                   "varint overflow",
                   0,
                   0},
        PrefixCase{
            {std::byte{3}, std::byte{0}, std::byte{0xff}}, "truncated length-prefixed value", 0, 0},
        PrefixCase{{std::byte{3}, std::byte{0}, std::byte{0xff}, std::byte{0x80}, std::byte{0x5a}},
                   {},
                   1,
                   3},
        PrefixCase{{std::byte{0}, std::byte{0x5a}}, {}, 1, 0},
        PrefixCase{{std::byte{0x80}, std::byte{0}, std::byte{0x5a}}, {}, 2, 0},
        PrefixCase{{std::byte{0x80}, std::byte{0x80}, std::byte{0x80}, std::byte{0x80},
                    std::byte{0x10}, std::byte{0x5a}},
                   {},
                   5,
                   0},
    };
    for (const auto& test : cases) {
        SCOPED_TRACE(test.bytes.size());
        const auto original = test.bytes;
        ByteView input = test.bytes;
        const auto result = ConsumeLengthPrefixed(input);
        if (!test.error.empty()) {
            ASSERT_FALSE(result.has_value());
            EXPECT_EQ(result.error().code(), ErrorCode::Corruption);
            EXPECT_EQ(result.error().message(), test.error);
            EXPECT_EQ(input.data(), test.bytes.data());
            EXPECT_EQ(input.size(), test.bytes.size());
            const std::string_view parent = test.error == "truncated length-prefixed value"
                                                ? "ConsumeLengthPrefixed"
                                                : "ConsumeVarint";
            EXPECT_TRUE(
                std::string_view(result.error().location().file_name()).ends_with("coding.cc"));
            EXPECT_NE(std::string_view(result.error().location().function_name()).find(parent),
                      std::string_view::npos);
        } else {
            ASSERT_TRUE(result.has_value());
            EXPECT_EQ(result->data(), test.bytes.data() + test.prefix_size);
            EXPECT_EQ(result->size(), test.payload_size);
            const ByteView expected =
                ByteView(test.bytes).subspan(test.prefix_size, test.payload_size);
            EXPECT_EQ(AsStringView(*result), AsStringView(expected));
            EXPECT_EQ(input.data(), result->data() + result->size());
            ASSERT_EQ(input.size(), 1U);
            EXPECT_EQ(input.front(), std::byte{0x5a});
        }
        EXPECT_EQ(test.bytes, original);
    }
}

TEST(CodingTest, EncodesAndDecodesFixedValuesInPreallocatedStorage) {
    std::array<std::byte, 12> storage;

    EncodeFixed64(std::span<std::byte, 8>(storage.data(), 8), 0x0102030405060708ULL);
    EncodeFixed32(std::span<std::byte, 4>(storage.data() + 8, 4), 0x0a0b0c0dU);

    EXPECT_EQ(storage, (std::array{
                           std::byte{0x08},
                           std::byte{0x07},
                           std::byte{0x06},
                           std::byte{0x05},
                           std::byte{0x04},
                           std::byte{0x03},
                           std::byte{0x02},
                           std::byte{0x01},
                           std::byte{0x0d},
                           std::byte{0x0c},
                           std::byte{0x0b},
                           std::byte{0x0a},
                       }));
    EXPECT_EQ(DecodeFixed64(std::span<const std::byte, 8>(storage.data(), 8)),
              0x0102030405060708ULL);
    EXPECT_EQ(DecodeFixed32(std::span<const std::byte, 4>(storage.data() + 8, 4)), 0x0a0b0c0dU);
}

TEST(CodingTest, EncodesVarint32IntoPreallocatedStorage) {
    std::array storage{
        std::byte{0xaa}, std::byte{0xaa}, std::byte{0xaa},
        std::byte{0xaa}, std::byte{0xaa}, std::byte{0xaa},
    };
    MutableByteView output = storage;

    ASSERT_TRUE(EncodeVarint32(output, 300));

    EXPECT_EQ(storage[0], std::byte{0xac});
    EXPECT_EQ(storage[1], std::byte{0x02});
    EXPECT_EQ(output.data(), storage.data() + 2);
    EXPECT_EQ(output.size(), storage.size() - 2U);
    EXPECT_EQ(storage[2], std::byte{0xaa});
}

TEST(CodingTest, ShortVarint32OutputIsUnchanged) {
    std::array storage{std::byte{0xaa}};
    MutableByteView output = storage;

    EXPECT_FALSE(EncodeVarint32(output, 128));

    EXPECT_EQ(output.data(), storage.data());
    EXPECT_EQ(output.size(), storage.size());
    EXPECT_EQ(storage[0], std::byte{0xaa});
}

TEST(CodingTest, EncodesFixed32InLittleEndianOrder) {
    std::vector<std::byte> output;

    AppendFixed32(output, 0x78563412U);

    EXPECT_EQ(output, (std::vector<std::byte>{
                          std::byte{0x12},
                          std::byte{0x34},
                          std::byte{0x56},
                          std::byte{0x78},
                      }));
}

TEST(CodingTest, AppendsFixedValuesWithoutChangingExistingBytes) {
    std::vector output{std::byte{0xaa}, std::byte{0xbb}};

    AppendFixed32(output, 0x78563412U);
    AppendFixed64(output, 0xfedcba9876543210ULL);

    EXPECT_EQ(output, (std::vector{
                          std::byte{0xaa},
                          std::byte{0xbb},
                          std::byte{0x12},
                          std::byte{0x34},
                          std::byte{0x56},
                          std::byte{0x78},
                          std::byte{0x10},
                          std::byte{0x32},
                          std::byte{0x54},
                          std::byte{0x76},
                          std::byte{0x98},
                          std::byte{0xba},
                          std::byte{0xdc},
                          std::byte{0xfe},
                      }));
}

TEST(CodingTest, DecodesFixed32AndConsumesOnlyItsBytes) {
    const std::vector input_storage{
        std::byte{0x12}, std::byte{0x34}, std::byte{0x56}, std::byte{0x78}, std::byte{0xff},
    };
    ByteView input = input_storage;

    const Result<std::uint32_t> value = ConsumeFixed32(input);

    ASSERT_TRUE(value.has_value());
    EXPECT_EQ(*value, 0x78563412U);
    ASSERT_EQ(input.size(), 1U);
    EXPECT_EQ(input.front(), std::byte{0xff});
}

TEST(CodingTest, RejectsTruncatedFixed32WithoutConsumingInput) {
    const std::vector input_storage{
        std::byte{0x12},
        std::byte{0x34},
        std::byte{0x56},
    };
    ByteView input = input_storage;

    const Result<std::uint32_t> value = ConsumeFixed32(input);

    ASSERT_FALSE(value.has_value());
    EXPECT_EQ(value.error().code(), ErrorCode::Corruption);
    EXPECT_EQ(value.error().message(), "truncated fixed32");
    EXPECT_EQ(input.data(), input_storage.data());
    EXPECT_EQ(input.size(), input_storage.size());
}

TEST(CodingTest, RoundTripsFixed64) {
    constexpr std::uint64_t Expected = 0xfedcba9876543210ULL;
    std::vector<std::byte> output;
    AppendFixed64(output, Expected);
    ByteView input = output;

    const Result<std::uint64_t> value = ConsumeFixed64(input);

    ASSERT_TRUE(value.has_value());
    EXPECT_EQ(*value, Expected);
    EXPECT_TRUE(input.empty());
}

TEST(CodingTest, MatchesFixed64GoldenVector) {
    constexpr std::uint64_t Value = 0xfedcba9876543210ULL;
    const std::vector<std::byte> golden{
        std::byte{0x10}, std::byte{0x32}, std::byte{0x54}, std::byte{0x76},
        std::byte{0x98}, std::byte{0xba}, std::byte{0xdc}, std::byte{0xfe},
    };
    std::vector<std::byte> output;
    AppendFixed64(output, Value);
    EXPECT_EQ(output, golden);

    for (std::size_t size = 0; size < golden.size(); ++size) {
        SCOPED_TRACE(size);
        ByteView input = ByteView(golden).first(size);
        const auto decoded = ConsumeFixed64(input);
        ASSERT_FALSE(decoded.has_value());
        EXPECT_EQ(decoded.error().code(), ErrorCode::Corruption);
        EXPECT_EQ(decoded.error().message(), "truncated fixed64");
        EXPECT_EQ(input.data(), golden.data());
        EXPECT_EQ(input.size(), size);
    }

    std::vector<std::byte> storage = golden;
    storage.push_back(std::byte{0x55});
    ByteView input = storage;
    const auto decoded = ConsumeFixed64(input);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(*decoded, Value);
    ASSERT_EQ(input.size(), 1U);
    EXPECT_EQ(input.front(), std::byte{0x55});
}

TEST(CodingTest, EncodesLevelDbCompatibleVarint32Values) {
    std::vector<std::byte> output;

    AppendVarint32(output, 0U);
    AppendVarint32(output, 127U);
    AppendVarint32(output, 128U);
    AppendVarint32(output, 300U);
    AppendVarint32(output, std::numeric_limits<std::uint32_t>::max());

    EXPECT_EQ(output, (std::vector<std::byte>{
                          std::byte{0x00},
                          std::byte{0x7f},
                          std::byte{0x80},
                          std::byte{0x01},
                          std::byte{0xac},
                          std::byte{0x02},
                          std::byte{0xff},
                          std::byte{0xff},
                          std::byte{0xff},
                          std::byte{0xff},
                          std::byte{0x0f},
                      }));
}

TEST(CodingTest, DecodesMaximumVarint32GoldenVector) {
    const std::vector<std::byte> golden{
        std::byte{0xff}, std::byte{0xff}, std::byte{0xff}, std::byte{0xff}, std::byte{0x0f},
    };
    for (std::size_t size = 0; size < golden.size(); ++size) {
        SCOPED_TRACE(size);
        ByteView input = ByteView(golden).first(size);
        const auto decoded = ConsumeVarint32(input);
        ASSERT_FALSE(decoded.has_value());
        EXPECT_EQ(decoded.error().code(), ErrorCode::Corruption);
        EXPECT_EQ(input.data(), golden.data());
        EXPECT_EQ(input.size(), size);
    }

    std::vector<std::byte> storage = golden;
    storage.push_back(std::byte{0x55});
    ByteView input = storage;
    const auto decoded = ConsumeVarint32(input);
    ASSERT_TRUE(decoded.has_value());
    EXPECT_EQ(*decoded, std::numeric_limits<std::uint32_t>::max());
    ASSERT_EQ(input.size(), 1U);
    EXPECT_EQ(input.front(), std::byte{0x55});
}

TEST(CodingTest, MatchesVarint64GoldenVectors) {
    struct GoldenVector {
        std::uint64_t value;
        std::vector<std::byte> bytes;
    };
    const std::vector<GoldenVector> vectors{
        {0U, {std::byte{0x00}}},
        {127U, {std::byte{0x7f}}},
        {128U, {std::byte{0x80}, std::byte{0x01}}},
        {std::uint64_t{1} << 32U,
         {std::byte{0x80}, std::byte{0x80}, std::byte{0x80}, std::byte{0x80}, std::byte{0x10}}},
        {std::numeric_limits<std::uint64_t>::max(),
         {std::byte{0xff}, std::byte{0xff}, std::byte{0xff}, std::byte{0xff}, std::byte{0xff},
          std::byte{0xff}, std::byte{0xff}, std::byte{0xff}, std::byte{0xff}, std::byte{0x01}}},
    };

    for (const auto& golden : vectors) {
        SCOPED_TRACE(golden.value);
        std::vector<std::byte> output;
        AppendVarint64(output, golden.value);
        EXPECT_EQ(output, golden.bytes);

        for (std::size_t size = 0; size < golden.bytes.size(); ++size) {
            SCOPED_TRACE(size);
            ByteView input = ByteView(golden.bytes).first(size);
            const auto decoded = ConsumeVarint64(input);
            ASSERT_FALSE(decoded.has_value());
            EXPECT_EQ(decoded.error().code(), ErrorCode::Corruption);
            EXPECT_EQ(input.data(), golden.bytes.data());
            EXPECT_EQ(input.size(), size);
        }

        std::vector<std::byte> storage = golden.bytes;
        storage.push_back(std::byte{0x55});
        ByteView input = storage;
        const auto decoded = ConsumeVarint64(input);
        ASSERT_TRUE(decoded.has_value());
        EXPECT_EQ(*decoded, golden.value);
        ASSERT_EQ(input.size(), 1U);
        EXPECT_EQ(input.front(), std::byte{0x55});
    }
}

TEST(CodingTest, AppendsVarintsWithoutChangingExistingBytes) {
    std::vector output{std::byte{0xaa}, std::byte{0xbb}};

    AppendVarint32(output, std::numeric_limits<std::uint32_t>::max());
    AppendVarint64(output, std::numeric_limits<std::uint64_t>::max());

    EXPECT_EQ(output, (std::vector{
                          std::byte{0xaa},
                          std::byte{0xbb},
                          std::byte{0xff},
                          std::byte{0xff},
                          std::byte{0xff},
                          std::byte{0xff},
                          std::byte{0x0f},
                          std::byte{0xff},
                          std::byte{0xff},
                          std::byte{0xff},
                          std::byte{0xff},
                          std::byte{0xff},
                          std::byte{0xff},
                          std::byte{0xff},
                          std::byte{0xff},
                          std::byte{0xff},
                          std::byte{0x01},
                      }));
}

TEST(CodingTest, RoundTripsVarint64Boundaries) {
    const std::vector<std::uint64_t> expected{
        0U,
        1U,
        127U,
        128U,
        16384U,
        std::numeric_limits<std::uint32_t>::max(),
        std::numeric_limits<std::uint64_t>::max(),
    };
    std::vector<std::byte> output;
    for (const std::uint64_t value : expected) {
        AppendVarint64(output, value);
    }
    ByteView input = output;

    for (const std::uint64_t value : expected) {
        const Result<std::uint64_t> decoded = ConsumeVarint64(input);
        ASSERT_TRUE(decoded.has_value());
        EXPECT_EQ(*decoded, value);
    }
    EXPECT_TRUE(input.empty());
}

TEST(CodingTest, RejectsTruncatedVarintWithoutConsumingInput) {
    const std::vector input_storage{std::byte{0x80}};
    ByteView input = input_storage;

    const Result<std::uint32_t> value = ConsumeVarint32(input);

    ASSERT_FALSE(value.has_value());
    EXPECT_EQ(value.error().code(), ErrorCode::Corruption);
    EXPECT_EQ(input.size(), input_storage.size());
}

TEST(CodingTest, TruncatesTerminalVarint32PayloadLikeLevelDb) {
    const std::vector input_storage{
        std::byte{0x80}, std::byte{0x80}, std::byte{0x80}, std::byte{0x80}, std::byte{0x10},
    };
    ByteView input = input_storage;

    const Result<std::uint32_t> value = ConsumeVarint32(input);

    ASSERT_TRUE(value.has_value());
    EXPECT_EQ(*value, 0U);
    EXPECT_TRUE(input.empty());
}

TEST(CodingTest, TruncatesTerminalVarint64PayloadLikeLevelDb) {
    const std::vector input_storage{
        std::byte{0xff}, std::byte{0xff}, std::byte{0xff}, std::byte{0xff}, std::byte{0xff},
        std::byte{0xff}, std::byte{0xff}, std::byte{0xff}, std::byte{0xff}, std::byte{0x02},
    };
    ByteView input = input_storage;

    const Result<std::uint64_t> value = ConsumeVarint64(input);

    ASSERT_TRUE(value.has_value());
    EXPECT_EQ(*value, std::numeric_limits<std::uint64_t>::max() >> 1U);
    EXPECT_TRUE(input.empty());
}

TEST(CodingTest, RejectsOverlongVarint32WithoutConsumingInput) {
    std::vector<std::byte> storage(6, std::byte{0x80});
    storage.back() = std::byte{0};
    ByteView input = storage;

    const auto decoded = ConsumeVarint32(input);

    ASSERT_FALSE(decoded.has_value());
    EXPECT_EQ(decoded.error().code(), ErrorCode::Corruption);
    EXPECT_EQ(input.data(), storage.data());
    EXPECT_EQ(input.size(), storage.size());
}

TEST(CodingTest, RejectsOverlongVarint64WithoutConsumingInput) {
    std::vector<std::byte> storage(11, std::byte{0x80});
    storage.back() = std::byte{0};
    ByteView input = storage;

    const auto decoded = ConsumeVarint64(input);

    ASSERT_FALSE(decoded.has_value());
    EXPECT_EQ(decoded.error().code(), ErrorCode::Corruption);
    EXPECT_EQ(input.data(), storage.data());
    EXPECT_EQ(input.size(), storage.size());
}

TEST(CodingTest, RoundTripsLengthPrefixedBytes) {
    std::vector<std::byte> output;
    AppendLengthPrefixed(output, AsBytes(std::string_view{"a\0b", 3}));
    output.push_back(std::byte{0xff});
    ByteView input = output;

    const Result<ByteView> value = ConsumeLengthPrefixed(input);

    ASSERT_TRUE(value.has_value());
    EXPECT_EQ(AsStringView(*value), std::string_view("a\0b", 3));
    ASSERT_EQ(input.size(), 1U);
    EXPECT_EQ(input.front(), std::byte{0xff});
}

TEST(CodingTest, AppendsEmptyLengthPrefixedBytes) {
    std::vector<std::byte> output;
    AppendLengthPrefixed(output, {});
    EXPECT_EQ(output, (std::vector<std::byte>{std::byte{0}}));

    output.push_back(std::byte{'x'});
    std::vector<std::byte> expected = output;
    expected.push_back(std::byte{0});

    AppendLengthPrefixed(output, {});

    EXPECT_EQ(output, expected);
}

TEST(CodingTest, RejectsTruncatedLengthPrefixedBytesWithoutConsumingInput) {
    const std::vector input_storage{
        std::byte{0x03},
        std::byte{'a'},
        std::byte{'b'},
    };
    ByteView input = input_storage;

    const Result<ByteView> value = ConsumeLengthPrefixed(input);

    ASSERT_FALSE(value.has_value());
    EXPECT_EQ(value.error().code(), ErrorCode::Corruption);
    EXPECT_EQ(input.size(), input_storage.size());
}

TEST(CodingTest, ComputesVarintLength) {
    EXPECT_EQ(VarintLength(0U), 1U);
    EXPECT_EQ(VarintLength(127U), 1U);
    EXPECT_EQ(VarintLength(128U), 2U);
    EXPECT_EQ(VarintLength(16384U), 3U);
    EXPECT_EQ(VarintLength(std::numeric_limits<std::uint64_t>::max()), 10U);
}

}  // namespace
}  // namespace modern_leveldb
