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

TEST(ArenaTest, StartsEmptyAndDoesNotReserveMemoryForZeroBytes) {
  Arena arena;

  EXPECT_EQ(arena.memory_usage(), 0U);
  EXPECT_TRUE(arena.Allocate(0).empty());
  EXPECT_TRUE(arena.AllocateAligned(0).empty());
  EXPECT_EQ(arena.memory_usage(), 0U);
}

TEST(ArenaTest, ReturnsWritableViewsWithExactRequestedSizes) {
  Arena arena;

  MutableByteView first = arena.Allocate(3);
  MutableByteView second = arena.Allocate(7);

  ASSERT_EQ(first.size(), 3U);
  ASSERT_EQ(second.size(), 7U);
  std::ranges::fill(first, std::byte{0x12});
  std::ranges::fill(second, std::byte{0x34});
  EXPECT_TRUE(std::ranges::all_of(first, [](std::byte byte) { return byte == std::byte{0x12}; }));
  EXPECT_TRUE(std::ranges::all_of(second, [](std::byte byte) { return byte == std::byte{0x34}; }));
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
    MutableByteView bytes = index % 7U == 0U ? arena.AllocateAligned(size) : arena.Allocate(size);
    std::ranges::fill(bytes, pattern);
    allocations.push_back({bytes, pattern});
  }

  for (const auto& allocation : allocations) {
    EXPECT_TRUE(std::ranges::all_of(allocation.bytes,
                                    [&](std::byte byte) { return byte == allocation.pattern; }));
  }
}

TEST(ArenaTest, ReturnsMaxAlignedStorageAfterUnalignedAllocations) {
  Arena arena;
  (void)arena.Allocate(1);

  for (std::size_t size = 1; size <= 64; ++size) {
    MutableByteView bytes = arena.AllocateAligned(size);
    ASSERT_EQ(bytes.size(), size);
    EXPECT_EQ(reinterpret_cast<std::uintptr_t>(bytes.data()) % alignof(std::max_align_t), 0U);
    std::ranges::fill(bytes, std::byte{0xa5});
  }
  EXPECT_EQ(arena.memory_usage(), 4'096U);
}

TEST(ArenaTest, UsesFourKilobyteBlocksForSmallAllocations) {
  Arena arena;

  for (int allocation = 0; allocation < 4; ++allocation) {
    (void)arena.Allocate(1'024);
    EXPECT_EQ(arena.memory_usage(), 4'096U);
  }

  (void)arena.Allocate(1);
  EXPECT_EQ(arena.memory_usage(), 8'192U);
}

TEST(ArenaTest, GivesLargeAllocationsDedicatedBlocksWithoutDiscardingSmallBlock) {
  Arena arena;
  MutableByteView current_tail;
  for (int allocation = 0; allocation < 3; ++allocation) {
    current_tail = arena.Allocate(1'024);
  }
  EXPECT_EQ(arena.memory_usage(), 4'096U);

  MutableByteView large = arena.Allocate(1'025);
  EXPECT_EQ(arena.memory_usage(), 5'121U);
  std::ranges::fill(large, std::byte{0x5a});

  MutableByteView small = arena.Allocate(8);
  EXPECT_EQ(arena.memory_usage(), 5'121U);
  EXPECT_EQ(small.data(), current_tail.data() + current_tail.size());
  EXPECT_TRUE(std::ranges::all_of(large, [](std::byte byte) { return byte == std::byte{0x5a}; }));
}

TEST(ArenaTest, UsesCurrentBlockWhenALargeRequestStillFits) {
  Arena arena;
  MutableByteView first = arena.Allocate(8);

  MutableByteView large = arena.Allocate(1'025);

  EXPECT_EQ(arena.memory_usage(), 4'096U);
  EXPECT_EQ(large.data(), first.data() + first.size());
}

TEST(ArenaTest, CountsOnlyOwnedBlockCapacity) {
  Arena arena;
  for (int allocation = 0; allocation < 4; ++allocation) {
    (void)arena.Allocate(1'024);
  }
  (void)arena.Allocate(1'025);
  (void)arena.Allocate(2'000);

  EXPECT_EQ(arena.memory_usage(), 4'096U + 1'025U + 2'000U);
}

}  // namespace
}  // namespace modern_leveldb
