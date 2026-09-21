#include "modern_leveldb/base/hash.h"

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

struct HashVector {
  std::uint32_t seed;
  std::array<std::uint32_t, BinaryInput.size() + 1> prefixes;
};

constexpr std::array GoldenVectors{
    HashVector{
        0x00000000U,
        {0x00000000U, 0x640c09b2U, 0xea1528d5U, 0x399e4691U, 0xaca7c9b6U, 0x477d84d6U, 0xd063c691U,
         0x990d4f84U, 0x72ae9b43U, 0x8cd89bf0U, 0xd0ee1ffbU, 0xc56cdf33U, 0x5f526d19U}},
    HashVector{
        0xbc9f1d34U,
        {0xbc9f1d34U, 0xc20e0a90U, 0xcb25c908U, 0x127d2876U, 0xc7e876bdU, 0x7fb3016dU, 0xd3382565U,
         0x65bb7174U, 0x9c75fd14U, 0xbeb24b88U, 0xccead32bU, 0x2fc294a5U, 0x361fc11bU}},
    HashVector{
        0x12345678U,
        {0x12345678U, 0x379a2269U, 0x029cce65U, 0xb0fc21f0U, 0xd2311f78U, 0x61a16639U, 0x477a0b5dU,
         0xd08261aaU, 0xc351a844U, 0x66e3a82dU, 0x40b1b7afU, 0x33673668U, 0xac50e65aU}},
};

static_assert(noexcept(Hash32(ByteView{}, 0U)));

TEST(HashTest, EmptyInputPreservesSeed) {
  for (const std::uint32_t seed : {0U, 1U, 0xffffffffU, 0xbc9f1d34U, 0x12345678U}) {
    SCOPED_TRACE(seed);
    EXPECT_EQ(Hash32({}, seed), seed);
  }
}

TEST(HashTest, MatchesUpstreamPrefixVectors) {
  for (const auto& golden : GoldenVectors) {
    SCOPED_TRACE(golden.seed);
    for (std::size_t length = 0; length <= BinaryInput.size(); ++length) {
      SCOPED_TRACE(length);
      EXPECT_EQ(Hash32(ByteView(BinaryInput).first(length), golden.seed), golden.prefixes[length]);
    }
  }
}

TEST(HashTest, AcceptsUnalignedPrefixes) {
  std::array<std::byte, BinaryInput.size() + 7> storage{};
  for (std::size_t offset = 0; offset < 8; ++offset) {
    SCOPED_TRACE(offset);
    std::ranges::copy(BinaryInput, storage.data() + offset);
    for (const auto& golden : GoldenVectors) {
      SCOPED_TRACE(golden.seed);
      for (std::size_t length = 0; length <= BinaryInput.size(); ++length) {
        SCOPED_TRACE(length);
        EXPECT_EQ(Hash32(ByteView(storage).subspan(offset, length), golden.seed),
                  golden.prefixes[length]);
      }
    }
  }
}

TEST(HashTest, PreservesCallerViewAndStorage) {
  auto storage = BinaryInput;
  ByteView input = storage;

  EXPECT_EQ(Hash32(input, GoldenVectors[0].seed), GoldenVectors[0].prefixes.back());
  EXPECT_EQ(input.data(), storage.data());
  EXPECT_EQ(input.size(), storage.size());
  EXPECT_EQ(storage, BinaryInput);
}

}  // namespace
}  // namespace modern_leveldb
