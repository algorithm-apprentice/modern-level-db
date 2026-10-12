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

constexpr std::size_t BoundaryInputSize = 19;

struct BoundaryHashVector {
    std::byte byte;
    std::uint32_t seed;
    std::array<std::uint32_t, BoundaryInputSize + 1> prefixes;
};

constexpr std::array BoundaryGoldenVectors{
    BoundaryHashVector{
        std::byte{0x00},
        0xffffffffU,
        {0xffffffffU, 0xb3523ab7U, 0x2d491bb6U, 0xa73ffd95U, 0x2136ffffU, 0xbceea88bU, 0x1f357a36U,
         0x1f316c90U, 0x176813fdU, 0x03efab91U, 0xfda1ce6fU, 0xf5964fbbU, 0x3ea35239U, 0x1d43df7dU,
         0xdc0cd594U, 0x2031dfe4U, 0x6010e04fU, 0x7d20dfbeU, 0xd4b08b5fU, 0xd798b17fU}},
    BoundaryHashVector{
        std::byte{0x00},
        0x80000000U,
        {0x80000000U, 0x06091e6fU, 0x8c123c5eU, 0x121b5b29U, 0x9824e180U, 0xb322e4d3U, 0xc1f43a01U,
         0x89837a29U, 0x49333033U, 0xf04ec87cU, 0x58f570f8U, 0x10fe79fdU, 0x3a3117bfU, 0xb8edf051U,
         0x258259e9U, 0x5aba53aaU, 0x3b7eaaa2U, 0x7ea54c3eU, 0x2a2bacb1U, 0x00c7a8f2U}},
    BoundaryHashVector{
        std::byte{0xff},
        0xffffffffU,
        {0xffffffffU, 0x915525e0U, 0x0e377406U, 0x739b55ecU, 0x5a926da4U, 0x40f39239U, 0x860c6068U,
         0x833d8de7U, 0x59e0e93aU, 0x3bda8478U, 0x1981debeU, 0x6e349478U, 0x967768e2U, 0xfaec39a2U,
         0xce62ffabU, 0x94c8d944U, 0x786b5750U, 0x0737d116U, 0xa7151680U, 0x3715910aU}},
    BoundaryHashVector{
        std::byte{0xff},
        0x80000000U,
        {0x80000000U, 0xe40c0932U, 0x6d009552U, 0xde76b376U, 0xd17f036eU, 0x0a123969U, 0xbe9079eeU,
         0x1ab3def8U, 0xc181beb1U, 0x5f536535U, 0xaac8be33U, 0xeefc5590U, 0x107bd398U, 0x8aee8182U,
         0x41d70e5bU, 0xaa269f38U, 0x2f051fe0U, 0x4bee0786U, 0x8ea22969U, 0x5df05596U}},
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
            EXPECT_EQ(Hash32(ByteView(BinaryInput).first(length), golden.seed),
                      golden.prefixes[length]);
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

TEST(HashTest, HighSeedsAndAllTailWidthsMatchLiteralBoundaryVectors) {
    for (const auto& golden : BoundaryGoldenVectors) {
        SCOPED_TRACE(golden.seed);
        SCOPED_TRACE(std::to_integer<unsigned int>(golden.byte));
        std::array<std::byte, BoundaryInputSize> storage{};
        std::ranges::fill(storage, golden.byte);
        for (std::size_t length = 0; length <= storage.size(); ++length) {
            SCOPED_TRACE(length);
            EXPECT_EQ(Hash32(ByteView(storage).first(length), golden.seed),
                      golden.prefixes[length]);
        }
    }
}

TEST(HashTest, GuardedSubspansPreserveCallerViewsAndStorage) {
    for (const auto& golden : BoundaryGoldenVectors) {
        SCOPED_TRACE(golden.seed);
        SCOPED_TRACE(std::to_integer<unsigned int>(golden.byte));
        for (std::size_t offset = 0; offset < 8; ++offset) {
            SCOPED_TRACE(offset);
            for (std::size_t length = 0; length <= BoundaryInputSize; ++length) {
                SCOPED_TRACE(length);
                std::array<std::byte, BoundaryInputSize + 8> storage{};
                std::ranges::fill(storage, std::byte{0x7e});
                std::ranges::fill(MutableByteView(storage).subspan(offset, length), golden.byte);
                const auto before = storage;
                const ByteView input = ByteView(storage).subspan(offset, length);

                EXPECT_EQ(Hash32(input, golden.seed), golden.prefixes[length]);
                EXPECT_EQ(input.data(), storage.data() + offset);
                EXPECT_EQ(input.size(), length);
                EXPECT_EQ(storage, before);
            }
        }
    }
}

}  // namespace
}  // namespace modern_leveldb
