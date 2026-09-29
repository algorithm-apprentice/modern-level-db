#include <gtest/gtest.h>

#include <cstddef>
#include <cstdlib>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <vector>

#if defined(_WIN32)
#include <malloc.h>
#endif

#include "cache/sharded_lru_cache.h"
#include "modern_leveldb/base/bytes.h"
#include "modern_leveldb/base/hash.h"

namespace {

struct AllocationSnapshot {
  std::size_t calls = 0;
  std::size_t bytes = 0;
  std::size_t successes_before_failure = std::numeric_limits<std::size_t>::max();
  void* last_allocation = nullptr;
};

struct DeallocationSnapshot {
  void* target = nullptr;
  std::size_t target_unsized_calls = 0;
  std::size_t target_sized_calls = 0;
};

thread_local AllocationSnapshot* active_allocations = nullptr;
thread_local DeallocationSnapshot* active_deallocations = nullptr;

void RecordAllocation(std::size_t bytes) noexcept {
  if (active_allocations != nullptr) {
    ++active_allocations->calls;
    active_allocations->bytes += bytes;
  }
}

void* Allocate(std::size_t bytes) {
  if (active_allocations != nullptr &&
      active_allocations->successes_before_failure != std::numeric_limits<std::size_t>::max()) {
    if (active_allocations->successes_before_failure == 0) {
      throw std::bad_alloc();
    }
    --active_allocations->successes_before_failure;
  }
  void* memory = std::malloc(bytes == 0 ? 1 : bytes);
  if (memory == nullptr) {
    throw std::bad_alloc();
  }
  RecordAllocation(bytes);
  if (active_allocations != nullptr) {
    active_allocations->last_allocation = memory;
  }
  return memory;
}

void* AllocateAligned(std::size_t bytes, std::size_t alignment) {
  if (active_allocations != nullptr &&
      active_allocations->successes_before_failure != std::numeric_limits<std::size_t>::max()) {
    if (active_allocations->successes_before_failure == 0) {
      throw std::bad_alloc();
    }
    --active_allocations->successes_before_failure;
  }
#if defined(_WIN32)
  void* memory = _aligned_malloc(bytes == 0 ? alignment : bytes, alignment);
  if (memory == nullptr) {
#else
  void* memory = nullptr;
  if (posix_memalign(&memory, alignment, bytes == 0 ? alignment : bytes) != 0) {
#endif
    throw std::bad_alloc();
  }
  RecordAllocation(bytes);
  if (active_allocations != nullptr) {
    active_allocations->last_allocation = memory;
  }
  return memory;
}

void FreeAligned(void* memory) noexcept {
#if defined(_WIN32)
  _aligned_free(memory);
#else
  std::free(memory);
#endif
}

class AllocationSession final {
 public:
  explicit AllocationSession(
      AllocationSnapshot& snapshot,
      std::size_t successes_before_failure = std::numeric_limits<std::size_t>::max()) {
    EXPECT_EQ(active_allocations, nullptr);
    snapshot = {};
    snapshot.successes_before_failure = successes_before_failure;
    active_allocations = &snapshot;
  }

  ~AllocationSession() { active_allocations = nullptr; }
};

class DeallocationSession final {
 public:
  DeallocationSession(DeallocationSnapshot& snapshot, void* target) {
    snapshot = {.target = target};
    active_deallocations = &snapshot;
  }

  ~DeallocationSession() { active_deallocations = nullptr; }
};

void RecordUnsizedDelete(void* memory) noexcept {
  if (active_deallocations != nullptr && memory == active_deallocations->target) {
    ++active_deallocations->target_unsized_calls;
  }
}

void RecordSizedDelete(void* memory) noexcept {
  if (active_deallocations != nullptr && memory == active_deallocations->target) {
    ++active_deallocations->target_sized_calls;
  }
}

}  // namespace

void* operator new(std::size_t bytes) { return Allocate(bytes); }
void* operator new[](std::size_t bytes) { return Allocate(bytes); }
void* operator new(std::size_t bytes, std::align_val_t alignment) {
  return AllocateAligned(bytes, static_cast<std::size_t>(alignment));
}
void* operator new[](std::size_t bytes, std::align_val_t alignment) {
  return AllocateAligned(bytes, static_cast<std::size_t>(alignment));
}
void* operator new(std::size_t bytes, const std::nothrow_t&) noexcept {
  try {
    return Allocate(bytes);
  } catch (...) {
    return nullptr;
  }
}
void* operator new[](std::size_t bytes, const std::nothrow_t&) noexcept {
  try {
    return Allocate(bytes);
  } catch (...) {
    return nullptr;
  }
}
void* operator new(std::size_t bytes, std::align_val_t alignment, const std::nothrow_t&) noexcept {
  try {
    return AllocateAligned(bytes, static_cast<std::size_t>(alignment));
  } catch (...) {
    return nullptr;
  }
}
void* operator new[](std::size_t bytes, std::align_val_t alignment,
                     const std::nothrow_t&) noexcept {
  try {
    return AllocateAligned(bytes, static_cast<std::size_t>(alignment));
  } catch (...) {
    return nullptr;
  }
}

void operator delete(void* memory) noexcept {
  RecordUnsizedDelete(memory);
  std::free(memory);
}
void operator delete[](void* memory) noexcept {
  RecordUnsizedDelete(memory);
  std::free(memory);
}
void operator delete(void* memory, std::size_t) noexcept {
  RecordSizedDelete(memory);
  std::free(memory);
}
void operator delete[](void* memory, std::size_t) noexcept {
  RecordSizedDelete(memory);
  std::free(memory);
}
void operator delete(void* memory, const std::nothrow_t&) noexcept { std::free(memory); }
void operator delete[](void* memory, const std::nothrow_t&) noexcept { std::free(memory); }
void operator delete(void* memory, std::align_val_t) noexcept { FreeAligned(memory); }
void operator delete[](void* memory, std::align_val_t) noexcept { FreeAligned(memory); }
void operator delete(void* memory, std::size_t, std::align_val_t) noexcept { FreeAligned(memory); }
void operator delete[](void* memory, std::size_t, std::align_val_t) noexcept {
  FreeAligned(memory);
}
void operator delete(void* memory, std::align_val_t, const std::nothrow_t&) noexcept {
  FreeAligned(memory);
}
void operator delete[](void* memory, std::align_val_t, const std::nothrow_t&) noexcept {
  FreeAligned(memory);
}

namespace modern_leveldb {
namespace {

struct TrackedValue {
  explicit TrackedValue(bool& destroyed) : destroyed_(&destroyed) {}
  ~TrackedValue() { *destroyed_ = true; }

 private:
  bool* destroyed_;
};

std::vector<std::string> KeysForShard(std::size_t shard, std::size_t count) {
  std::vector<std::string> keys;
  for (std::uint64_t candidate = 0; keys.size() < count; ++candidate) {
    const std::string key = "allocation-key-" + std::to_string(candidate);
    if ((Hash32(AsBytes(key), 0U) >> 28U) == shard) {
      keys.push_back(key);
    }
  }
  return keys;
}

TEST(CacheAllocationTest, InsertUsesOneEntryAllocationAndLookupUsesNone) {
  ShardedLruCache<int> cache(16);
  std::optional<ShardedLruCache<int>::Handle> inserted;
  AllocationSnapshot insert_allocations;
  {
    auto value = std::make_unique<const int>(42);
    AllocationSession session(insert_allocations);
    Result<ShardedLruCache<int>::Handle> result =
        cache.Insert(AsBytes("inline-key"), std::move(value), 1);
    if (result.has_value()) {
      inserted.emplace(std::move(*result));
    }
  }

  ASSERT_TRUE(inserted.has_value());
  EXPECT_EQ(insert_allocations.calls, 1U);
  EXPECT_GT(insert_allocations.bytes, sizeof(int));

  std::optional<ShardedLruCache<int>::Handle> found;
  AllocationSnapshot lookup_allocations;
  {
    AllocationSession session(lookup_allocations);
    found = cache.Lookup(AsBytes("inline-key"));
  }

  ASSERT_TRUE(found.has_value());
  EXPECT_EQ(lookup_allocations.calls, 0U);
  EXPECT_EQ(lookup_allocations.bytes, 0U);
  EXPECT_EQ(**found, 42);
}

TEST(CacheAllocationTest, FailedBucketGrowthLeavesMappingsAndChargeUnchanged) {
  ShardedLruCache<int> cache(16 * 16);
  const std::vector<std::string> keys = KeysForShard(0, 5);
  for (std::size_t index = 0; index < 4; ++index) {
    ASSERT_TRUE(
        cache.Insert(AsBytes(keys[index]), std::make_unique<const int>(static_cast<int>(index)), 1)
            .has_value());
  }
  ASSERT_EQ(cache.total_charge(), 4U);

  bool allocation_failed = false;
  AllocationSnapshot failed_insert;
  {
    auto value = std::make_unique<const int>(4);
    AllocationSession session(failed_insert, 1);
    try {
      static_cast<void>(cache.Insert(AsBytes(keys[4]), std::move(value), 1));
    } catch (const std::bad_alloc&) {
      allocation_failed = true;
    }
  }

  EXPECT_TRUE(allocation_failed);
  EXPECT_EQ(failed_insert.calls, 1U);
  EXPECT_EQ(cache.total_charge(), 4U);
  EXPECT_FALSE(cache.Lookup(AsBytes(keys[4])).has_value());
  for (std::size_t index = 0; index < 4; ++index) {
    auto found = cache.Lookup(AsBytes(keys[index]));
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(**found, static_cast<int>(index));
  }
}

TEST(CacheAllocationTest, FailedEntryAllocationDestroysTransferredValue) {
  ShardedLruCache<TrackedValue> cache(16);
  bool destroyed = false;
  bool allocation_failed = false;
  AllocationSnapshot failed_insert;
  {
    auto value = std::make_unique<const TrackedValue>(destroyed);
    AllocationSession session(failed_insert, 0);
    try {
      static_cast<void>(cache.Insert(AsBytes("failed-entry"), std::move(value), 1));
    } catch (const std::bad_alloc&) {
      allocation_failed = true;
    }
  }

  EXPECT_TRUE(allocation_failed);
  EXPECT_TRUE(destroyed);
  EXPECT_EQ(failed_insert.calls, 0U);
  EXPECT_EQ(cache.total_charge(), 0U);
  EXPECT_FALSE(cache.Lookup(AsBytes("failed-entry")).has_value());
}

TEST(CacheAllocationTest, VariableSizeEntryUsesUnsizedDeallocation) {
  ShardedLruCache<int> cache(16);
  std::optional<ShardedLruCache<int>::Handle> inserted;
  AllocationSnapshot allocation;
  {
    auto value = std::make_unique<const int>(42);
    AllocationSession session(allocation);
    Result<ShardedLruCache<int>::Handle> result =
        cache.Insert(AsBytes("variable-size-entry"), std::move(value), 1);
    if (result.has_value()) {
      inserted.emplace(std::move(*result));
    }
  }
  ASSERT_TRUE(inserted.has_value());
  ASSERT_NE(allocation.last_allocation, nullptr);
  cache.Erase(AsBytes("variable-size-entry"));

  DeallocationSnapshot deallocation;
  {
    DeallocationSession session(deallocation, allocation.last_allocation);
    inserted.reset();
  }

  EXPECT_EQ(deallocation.target_unsized_calls, 1U);
  EXPECT_EQ(deallocation.target_sized_calls, 0U);
}

}  // namespace
}  // namespace modern_leveldb
