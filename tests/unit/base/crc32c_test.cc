#include "modern_leveldb/base/crc32c.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

#include "modern_leveldb/base/bytes.h"

namespace modern_leveldb {
namespace {

constexpr std::array<std::byte, 12> BinaryInput{
    std::byte{0xff}, std::byte{0x00}, std::byte{0x80}, std::byte{0x7f},
    std::byte{0x01}, std::byte{0xfe}, std::byte{0x02}, std::byte{0x81},
    std::byte{0x03}, std::byte{0xfd}, std::byte{0x04}, std::byte{0x82},
};
constexpr std::uint32_t BinaryChecksum = 0xbe08f044U;

struct MaskVector {
  std::uint32_t crc;
  std::uint32_t masked;
};

constexpr std::array MaskVectors{
    MaskVector{0x00000000U, 0xa282ead8U}, MaskVector{0x00000001U, 0xa284ead8U},
    MaskVector{0x80000000U, 0xa283ead8U}, MaskVector{0xffffffffU, 0xa282ead7U},
    MaskVector{0xe3069283U, 0xc78ab0e5U},
};

std::uint32_t BitwiseCrc32c(ByteView input) {
  std::uint32_t crc = 0xffffffffU;
  for (const std::byte byte : input) {
    crc ^= std::to_integer<std::uint32_t>(byte);
    for (int bit = 0; bit < 8; ++bit) {
      crc = (crc & 1U) != 0U ? (crc >> 1U) ^ 0x82f63b78U : crc >> 1U;
    }
  }
  return ~crc;
}

static_assert(noexcept(Crc32c(ByteView{})));
static_assert(noexcept(ExtendCrc32c(0U, ByteView{})));
static_assert(noexcept(MaskCrc32c(0U)));
static_assert(noexcept(UnmaskCrc32c(0U)));

TEST(Crc32cTest, EmptyInputHasZeroChecksum) { EXPECT_EQ(Crc32c({}), 0U); }

TEST(Crc32cTest, MatchesStandardCheckString) {
  EXPECT_EQ(Crc32c(AsBytes("123456789")), 0xe3069283U);
}

TEST(Crc32cTest, MatchesRfc3720Vectors) {
  std::array<std::byte, 32> bytes{};
  EXPECT_EQ(Crc32c(bytes), 0x8a9136aaU);

  bytes.fill(std::byte{0xff});
  EXPECT_EQ(Crc32c(bytes), 0x62a8ab43U);

  for (std::size_t index = 0; index < bytes.size(); ++index) {
    bytes[index] = static_cast<std::byte>(index);
  }
  EXPECT_EQ(Crc32c(bytes), 0x46dd794eU);

  std::ranges::reverse(bytes);
  EXPECT_EQ(Crc32c(bytes), 0x113fdb5cU);
}

TEST(Crc32cTest, MatchesUpstreamBinaryVector) { EXPECT_EQ(Crc32c(BinaryInput), BinaryChecksum); }

TEST(Crc32cTest, MatchesBitwiseReferenceForAllByteValues) {
  for (unsigned int value = 0; value <= 0xffU; ++value) {
    SCOPED_TRACE(value);
    const auto byte = static_cast<std::byte>(value);
    const ByteView input(&byte, 1);
    EXPECT_EQ(Crc32c(input), BitwiseCrc32c(input));
  }
}

TEST(Crc32cTest, ExtendsEverySplitOfBinaryInput) {
  const ByteView input = BinaryInput;
  for (std::size_t split = 0; split <= input.size(); ++split) {
    SCOPED_TRACE(split);
    const std::uint32_t initial = Crc32c(input.first(split));
    EXPECT_EQ(ExtendCrc32c(initial, input.subspan(split)), BinaryChecksum);
  }
}

TEST(Crc32cTest, ExtendsOneByteAtATime) {
  std::uint32_t crc = 0;
  for (const std::byte& byte : BinaryInput) {
    crc = ExtendCrc32c(crc, ByteView(&byte, 1));
  }
  EXPECT_EQ(crc, BinaryChecksum);
}

TEST(Crc32cTest, EmptyExtensionPreservesFinalizedChecksum) {
  for (const std::uint32_t crc : {0U, 1U, 0xffffffffU, 0x12345678U}) {
    SCOPED_TRACE(crc);
    EXPECT_EQ(ExtendCrc32c(crc, {}), crc);
  }
}

TEST(Crc32cTest, ExtendsArbitraryFinalizedChecksum) {
  EXPECT_EQ(ExtendCrc32c(0x12345678U, BinaryInput), 0x045c8a09U);
}

TEST(Crc32cTest, MatchesLevelDbMaskVectors) {
  for (const auto& [crc, masked] : MaskVectors) {
    SCOPED_TRACE(crc);
    EXPECT_EQ(MaskCrc32c(crc), masked);
  }
}

TEST(Crc32cTest, MatchesLevelDbUnmaskVectors) {
  for (const auto& [crc, masked] : MaskVectors) {
    SCOPED_TRACE(masked);
    EXPECT_EQ(UnmaskCrc32c(masked), crc);
  }
}

TEST(Crc32cTest, MaskAndUnmaskAreInversesForEveryBit) {
  for (unsigned int bit = 0; bit < 32U; ++bit) {
    SCOPED_TRACE(bit);
    const std::uint32_t value = std::uint32_t{1} << bit;
    EXPECT_EQ(UnmaskCrc32c(MaskCrc32c(value)), value);
    EXPECT_EQ(MaskCrc32c(UnmaskCrc32c(value)), value);
    EXPECT_EQ(UnmaskCrc32c(MaskCrc32c(~value)), ~value);
  }
}

TEST(Crc32cTest, AcceptsUnalignedViewsWithoutConsumingOrChangingInput) {
  std::array<std::byte, BinaryInput.size() + 1> storage{};
  std::ranges::copy(BinaryInput, storage.begin() + 1);
  const auto original = storage;
  ByteView input = ByteView(storage).subspan(1);

  EXPECT_EQ(Crc32c(input), BinaryChecksum);
  EXPECT_EQ(ExtendCrc32c(0U, input), BinaryChecksum);
  EXPECT_EQ(input.data(), storage.data() + 1);
  EXPECT_EQ(input.size(), BinaryInput.size());
  EXPECT_EQ(storage, original);
}

}  // namespace
}  // namespace modern_leveldb
