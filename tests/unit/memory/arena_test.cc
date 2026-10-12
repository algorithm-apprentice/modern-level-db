#include "memory/arena.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <type_traits>
#include <utility>
#include <vector>

#include "modern_leveldb/base/bytes.h"

namespace modern_leveldb {
namespace {

static_assert(!std::is_copy_constructible_v<Arena>);
static_assert(!std::is_copy_assignable_v<Arena>);
static_assert(!std::is_move_constructible_v<Arena>);
static_assert(!std::is_move_assignable_v<Arena>);
static_assert(noexcept(std::declval<const Arena&>().memory_usage()));
static_assert(Arena::Alignment == std::max(sizeof(void*), std::size_t{8}));

constexpr std::size_t BlockCharge(std::size_t bytes) noexcept { return bytes + sizeof(std::byte*); }

MutableByteView AllocateView(Arena& arena, std::size_t size, bool aligned = false) {
    std::byte* storage = aligned ? arena.AllocateAligned(size) : arena.Allocate(size);
    return MutableByteView(storage, size);
}

TEST(ArenaTest, StartsEmpty) {
    Arena arena;

    EXPECT_EQ(arena.memory_usage(), 0U);
}

TEST(ArenaTest, ReturnedStorageSupportsRequestedWritableViews) {
    Arena arena;

    MutableByteView first = AllocateView(arena, 3);
    MutableByteView second = AllocateView(arena, 7);

    ASSERT_EQ(first.size(), 3U);
    ASSERT_EQ(second.size(), 7U);
    std::ranges::fill(first, std::byte{0x12});
    std::ranges::fill(second, std::byte{0x34});
    EXPECT_TRUE(std::ranges::all_of(first, [](std::byte byte) { return byte == std::byte{0x12}; }));
    EXPECT_TRUE(
        std::ranges::all_of(second, [](std::byte byte) { return byte == std::byte{0x34}; }));
}

TEST(ArenaTest, PreservesAllAllocationsAcrossBlockGrowth) {
    struct Allocation {
        MutableByteView bytes;
        std::byte pattern;
    };

    Arena arena;
    std::vector<Allocation> allocations;
    allocations.reserve(2'000);
    for (std::size_t index = 0; index < 2'000; ++index) {
        const std::size_t size = index % 131U + 1U;
        const auto pattern = static_cast<std::byte>(index % 251U);
        MutableByteView bytes = AllocateView(arena, size, index % 7U == 0U);
        std::ranges::fill(bytes, pattern);
        allocations.push_back({bytes, pattern});
    }

    for (const auto& allocation : allocations) {
        EXPECT_TRUE(std::ranges::all_of(
            allocation.bytes, [&](std::byte byte) { return byte == allocation.pattern; }));
    }
}

TEST(ArenaTest, ReturnsLevelDbAlignedStorageAfterUnalignedAllocations) {
    Arena arena;
    (void)arena.Allocate(1);

    for (std::size_t size = 1; size <= 64; ++size) {
        MutableByteView bytes = AllocateView(arena, size, true);
        ASSERT_EQ(bytes.size(), size);
        EXPECT_EQ(reinterpret_cast<std::uintptr_t>(bytes.data()) % Arena::Alignment, 0U);
        std::ranges::fill(bytes, std::byte{0xa5});
    }
    EXPECT_EQ(arena.memory_usage(), BlockCharge(4'096U));
}

TEST(ArenaTest, ExactPaddedFitStaysInBlockAndOneByteOverUsesDedicatedStorage) {
    Arena exact_fit;
    std::byte* exact_prefix = exact_fit.Allocate(1);
    *exact_prefix = std::byte{0xa5};

    MutableByteView exact = AllocateView(exact_fit, 4'088, true);

    EXPECT_EQ(exact.data(), exact_prefix + 8);
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(exact.data()) % Arena::Alignment, 0U);
    EXPECT_EQ(exact_fit.memory_usage(), BlockCharge(4'096U));
    EXPECT_EQ(*exact_prefix, std::byte{0xa5});

    Arena one_over;
    std::byte* over_prefix = one_over.Allocate(1);
    *over_prefix = std::byte{0x5a};

    MutableByteView over = AllocateView(one_over, 4'089, true);

    EXPECT_NE(over.data(), over_prefix + 8);
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(over.data()) % Arena::Alignment, 0U);
    EXPECT_EQ(one_over.memory_usage(), BlockCharge(4'096U) + BlockCharge(4'089U));
    EXPECT_EQ(*over_prefix, std::byte{0x5a});

    MutableByteView preserved_current = AllocateView(one_over, 1);
    EXPECT_EQ(preserved_current.data(), over_prefix + 1);
    EXPECT_EQ(one_over.memory_usage(), BlockCharge(4'096U) + BlockCharge(4'089U));
}

TEST(ArenaTest, UsesFourKilobyteBlocksForSmallAllocations) {
    Arena arena;

    for (int allocation = 0; allocation < 4; ++allocation) {
        (void)arena.Allocate(1'024);
        EXPECT_EQ(arena.memory_usage(), BlockCharge(4'096U));
    }

    (void)arena.Allocate(1);
    EXPECT_EQ(arena.memory_usage(), 2U * BlockCharge(4'096U));
}

TEST(ArenaTest, GivesLargeAllocationsDedicatedBlocksWithoutDiscardingSmallBlock) {
    Arena arena;
    MutableByteView current_tail;
    for (int allocation = 0; allocation < 3; ++allocation) {
        current_tail = AllocateView(arena, 1'024);
    }
    EXPECT_EQ(arena.memory_usage(), BlockCharge(4'096U));

    MutableByteView large = AllocateView(arena, 1'025);
    EXPECT_EQ(arena.memory_usage(), BlockCharge(4'096U) + BlockCharge(1'025U));
    std::ranges::fill(large, std::byte{0x5a});

    MutableByteView small = AllocateView(arena, 8);
    EXPECT_EQ(arena.memory_usage(), BlockCharge(4'096U) + BlockCharge(1'025U));
    EXPECT_EQ(small.data(), current_tail.data() + current_tail.size());
    EXPECT_TRUE(std::ranges::all_of(large, [](std::byte byte) { return byte == std::byte{0x5a}; }));
}

TEST(ArenaTest, UsesCurrentBlockWhenALargeRequestStillFits) {
    Arena arena;
    MutableByteView first = AllocateView(arena, 8);

    MutableByteView large = AllocateView(arena, 1'025);

    EXPECT_EQ(arena.memory_usage(), BlockCharge(4'096U));
    EXPECT_EQ(large.data(), first.data() + first.size());
}

TEST(ArenaTest, CountsOwnedBlockCapacityAndOwnerPointers) {
    Arena arena;
    for (int allocation = 0; allocation < 4; ++allocation) {
        (void)arena.Allocate(1'024);
    }
    (void)arena.Allocate(1'025);
    (void)arena.Allocate(2'000);

    EXPECT_EQ(arena.memory_usage(),
              BlockCharge(4'096U) + BlockCharge(1'025U) + BlockCharge(2'000U));
}

}  // namespace
}  // namespace modern_leveldb
