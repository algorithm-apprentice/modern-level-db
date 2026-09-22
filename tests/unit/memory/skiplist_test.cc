#include "memory/skiplist.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <latch>
#include <set>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "memory/arena.h"
#include "modern_leveldb/base/bytes.h"
#include "modern_leveldb/base/comparator.h"

namespace modern_leveldb {
namespace {

struct IntegerCompare {
  int operator()(std::uint64_t left, std::uint64_t right) const noexcept {
    if (left < right) {
      return -1;
    }
    if (left > right) {
      return 1;
    }
    return 0;
  }
};

struct ByteViewCompare {
  int operator()(ByteView left, ByteView right) const noexcept {
    return BytewiseComparator().Compare(left, right);
  }
};

struct alignas(64) OverAlignedKey {
  std::uint64_t value;
};

struct OverAlignedCompare {
  int operator()(const OverAlignedKey& left, const OverAlignedKey& right) const noexcept {
    return IntegerCompare{}(left.value, right.value);
  }
};

struct ConcurrentKey {
  std::uint64_t order;
  std::uint64_t checksum;
};

struct ConcurrentKeyCompare {
  int operator()(const ConcurrentKey& left, const ConcurrentKey& right) const noexcept {
    return IntegerCompare{}(left.order, right.order);
  }
};

ConcurrentKey MakeConcurrentKey(std::uint64_t order) {
  return ConcurrentKey{
      .order = order,
      .checksum = (order * 0x9e3779b97f4a7c15ULL) ^ 0xd6e8feb86659fd93ULL,
  };
}

bool IsValidConcurrentKey(const ConcurrentKey& key) {
  return key.checksum == MakeConcurrentKey(key.order).checksum;
}

using IntegerList = SkipList<std::uint64_t, IntegerCompare>;

template <typename Key, typename Compare>
concept HasSkipList = requires { typename SkipList<Key, Compare>; };

static_assert(!std::is_copy_constructible_v<IntegerList>);
static_assert(!std::is_copy_assignable_v<IntegerList>);
static_assert(!std::is_move_constructible_v<IntegerList>);
static_assert(!std::is_move_assignable_v<IntegerList>);
static_assert(!std::is_constructible_v<IntegerList, IntegerCompare&&, Arena&>);
static_assert(!std::is_constructible_v<IntegerList, const IntegerCompare&&, Arena&>);
static_assert(!HasSkipList<OverAlignedKey, OverAlignedCompare>);

TEST(SkipListTest, EmptyListAndIteratorAreInvalid) {
  Arena arena;
  IntegerCompare compare;
  IntegerList list(compare, arena);
  IntegerList::Iterator iterator(list);

  EXPECT_FALSE(list.Contains(10));
  EXPECT_FALSE(iterator.valid());
  iterator.SeekToFirst();
  EXPECT_FALSE(iterator.valid());
  iterator.SeekToLast();
  EXPECT_FALSE(iterator.valid());
  iterator.Seek(10);
  EXPECT_FALSE(iterator.valid());
}

TEST(SkipListTest, InsertsAndRejectsDuplicates) {
  Arena arena;
  IntegerCompare compare;
  IntegerList list(compare, arena);

  EXPECT_TRUE(list.Insert(10));
  EXPECT_TRUE(list.Insert(5));
  EXPECT_TRUE(list.Insert(20));
  EXPECT_FALSE(list.Insert(10));
  EXPECT_TRUE(list.Contains(5));
  EXPECT_TRUE(list.Contains(10));
  EXPECT_TRUE(list.Contains(20));
  EXPECT_FALSE(list.Contains(15));
}

TEST(SkipListTest, IteratesSeeksAndMovesBackward) {
  Arena arena;
  IntegerCompare compare;
  IntegerList list(compare, arena);
  for (const std::uint64_t key : std::array<std::uint64_t, 4>{30, 10, 40, 20}) {
    ASSERT_TRUE(list.Insert(key));
  }

  IntegerList::Iterator iterator(list);
  iterator.SeekToFirst();
  for (const std::uint64_t expected : std::array<std::uint64_t, 4>{10, 20, 30, 40}) {
    ASSERT_TRUE(iterator.valid());
    EXPECT_EQ(iterator.key(), expected);
    iterator.Next();
  }
  EXPECT_FALSE(iterator.valid());

  iterator.Seek(25);
  ASSERT_TRUE(iterator.valid());
  EXPECT_EQ(iterator.key(), 30U);
  iterator.Seek(40);
  ASSERT_TRUE(iterator.valid());
  EXPECT_EQ(iterator.key(), 40U);
  iterator.Seek(41);
  EXPECT_FALSE(iterator.valid());

  iterator.SeekToLast();
  for (const std::uint64_t expected : std::array<std::uint64_t, 4>{40, 30, 20, 10}) {
    ASSERT_TRUE(iterator.valid());
    EXPECT_EQ(iterator.key(), expected);
    iterator.Prev();
  }
  EXPECT_FALSE(iterator.valid());
}

TEST(SkipListTest, MatchesRandomizedSetModel) {
  Arena arena;
  IntegerCompare compare;
  IntegerList list(compare, arena);
  std::set<std::uint64_t> model;
  std::uint32_t random = 0x12345678U;

  for (int insertion = 0; insertion < 2'000; ++insertion) {
    random = random * 1664525U + 1013904223U;
    const std::uint64_t key = random % 5'000U;
    EXPECT_EQ(list.Insert(key), model.insert(key).second);
  }

  for (std::uint64_t target = 0; target < 5'000; target += 17) {
    EXPECT_EQ(list.Contains(target), model.contains(target));
    IntegerList::Iterator iterator(list);
    iterator.Seek(target);
    const auto expected = model.lower_bound(target);
    if (expected == model.end()) {
      EXPECT_FALSE(iterator.valid());
    } else {
      ASSERT_TRUE(iterator.valid());
      EXPECT_EQ(iterator.key(), *expected);
    }
  }

  IntegerList::Iterator iterator(list);
  iterator.SeekToFirst();
  for (const std::uint64_t expected : model) {
    ASSERT_TRUE(iterator.valid());
    EXPECT_EQ(iterator.key(), expected);
    iterator.Next();
  }
  EXPECT_FALSE(iterator.valid());
}

TEST(SkipListTest, StoresArenaBackedBinaryViews) {
  Arena arena;
  ByteViewCompare compare;
  SkipList<ByteView, ByteViewCompare> list(compare, arena);

  auto allocate_key = [&](std::string_view text) {
    MutableByteView storage = arena.Allocate(text.size());
    std::ranges::copy(AsBytes(text), storage.begin());
    return ByteView(storage);
  };

  const ByteView alpha = allocate_key(std::string_view("a\0x", 3));
  const ByteView beta = allocate_key(std::string_view("b\0y", 3));
  ASSERT_TRUE(list.Insert(beta));
  ASSERT_TRUE(list.Insert(alpha));

  SkipList<ByteView, ByteViewCompare>::Iterator iterator(list);
  iterator.SeekToFirst();
  ASSERT_TRUE(iterator.valid());
  EXPECT_TRUE(std::ranges::equal(iterator.key(), alpha));
  iterator.Next();
  ASSERT_TRUE(iterator.valid());
  EXPECT_TRUE(std::ranges::equal(iterator.key(), beta));
}

TEST(SkipListTest, ConcurrentReadersObserveOnlyInitializedOrderedNodes) {
  constexpr std::uint64_t InsertedCount = 4'096;
  constexpr std::uint64_t Modulus = 4'099;
  constexpr int ReaderCount = 4;

  Arena arena;
  ConcurrentKeyCompare compare;
  SkipList<ConcurrentKey, ConcurrentKeyCompare> list(compare, arena);
  ASSERT_TRUE(list.Insert(MakeConcurrentKey(0)));
  ASSERT_TRUE(list.Insert(MakeConcurrentKey(Modulus)));

  std::atomic<bool> done = false;
  std::atomic<int> failures = 0;
  std::atomic<int> read_iterations = 0;
  std::latch readers_ready(ReaderCount);
  std::vector<std::jthread> readers;

  for (int reader = 0; reader < ReaderCount; ++reader) {
    readers.emplace_back([&] {
      auto scan = [&] {
        SkipList<ConcurrentKey, ConcurrentKeyCompare>::Iterator iterator(list);
        iterator.SeekToFirst();
        bool first = true;
        std::uint64_t previous = 0;
        while (iterator.valid()) {
          const ConcurrentKey& key = iterator.key();
          if (!IsValidConcurrentKey(key) || (!first && key.order <= previous)) {
            failures.fetch_add(1, std::memory_order_relaxed);
            break;
          }
          first = false;
          previous = key.order;
          iterator.Next();
        }
        read_iterations.fetch_add(1, std::memory_order_relaxed);
      };

      scan();
      readers_ready.count_down();
      while (!done.load(std::memory_order_acquire)) {
        scan();
      }
    });
  }

  readers_ready.wait();
  for (std::uint64_t index = 1; index <= InsertedCount; ++index) {
    const std::uint64_t order = (index * 37U) % Modulus;
    ASSERT_TRUE(list.Insert(MakeConcurrentKey(order)));
    if (index % 64U == 0U) {
      std::this_thread::yield();
    }
  }
  done.store(true, std::memory_order_release);
  readers.clear();

  EXPECT_EQ(failures.load(std::memory_order_relaxed), 0);
  EXPECT_GE(read_iterations.load(std::memory_order_relaxed), ReaderCount);

  SkipList<ConcurrentKey, ConcurrentKeyCompare>::Iterator iterator(list);
  iterator.SeekToFirst();
  std::size_t final_count = 0;
  std::uint64_t previous = 0;
  while (iterator.valid()) {
    const ConcurrentKey& key = iterator.key();
    EXPECT_TRUE(IsValidConcurrentKey(key));
    if (final_count > 0) {
      EXPECT_GT(key.order, previous);
    }
    previous = key.order;
    ++final_count;
    iterator.Next();
  }
  EXPECT_EQ(final_count, InsertedCount + 2U);
  for (std::uint64_t index = 1; index <= InsertedCount; ++index) {
    const std::uint64_t order = (index * 37U) % Modulus;
    EXPECT_TRUE(list.Contains(MakeConcurrentKey(order)));
  }
  EXPECT_TRUE(list.Contains(MakeConcurrentKey(0)));
  EXPECT_TRUE(list.Contains(MakeConcurrentKey(Modulus)));
  EXPECT_GT(arena.memory_usage(), 0U);
}

}  // namespace
}  // namespace modern_leveldb
