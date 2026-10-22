#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdlib>
#include <exception>
#include <filesystem>
#include <latch>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#if defined(_WIN32)
#include <malloc.h>
#endif

#include "cache/sharded_lru_cache.h"
#include "engine/table_cache.h"
#include "format/internal_key.h"
#include "metadata/filenames.h"
#include "modern_leveldb/base/bytes.h"
#include "modern_leveldb/base/coding.h"
#include "modern_leveldb/base/hash.h"
#include "support/memory_file_system.h"
#include "table/table_builder.h"

namespace {

struct AllocationSnapshot {
    std::size_t calls = 0;
    std::size_t bytes = 0;
    std::size_t successes_before_failure = std::numeric_limits<std::size_t>::max();
    void* last_allocation = nullptr;
    std::array<void*, 128> addresses{};
    std::array<std::size_t, 128> sizes{};
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
        if (active_allocations->calls < active_allocations->addresses.size()) {
            active_allocations->addresses[active_allocations->calls] =
                active_allocations->last_allocation;
            active_allocations->sizes[active_allocations->calls] = bytes;
        }
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
    if (active_allocations != nullptr) {
        active_allocations->last_allocation = memory;
    }
    RecordAllocation(bytes);
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
    if (active_allocations != nullptr) {
        active_allocations->last_allocation = memory;
    }
    RecordAllocation(bytes);
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

class ReleaseLatch {
public:
    explicit ReleaseLatch(std::latch& latch) : latch_(latch) {}
    ~ReleaseLatch() { Open(); }

    void Open() {
        if (std::exchange(active_, false)) {
            latch_.count_down();
        }
    }

private:
    std::latch& latch_;
    bool active_ = true;
};

std::vector<std::uint64_t> TableFilesForOneShard() {
    std::vector<std::uint64_t> files;
    for (std::uint64_t number = 1; files.size() < 3; ++number) {
        std::array<std::byte, 8> key{};
        EncodeFixed64(key, number);
        if ((Hash32(key, 0) >> 28U) == 0) {
            files.push_back(number);
        }
    }
    return files;
}

std::uint64_t WriteAllocationTable(test_support::MemoryFileSystem& file_system,
                                   const InternalKeyComparator& comparator, std::uint64_t number) {
    auto file = file_system.OpenWritable(TableFileName("db", number)).value();
    TableBuilder builder(std::move(file), comparator, {});
    const auto key = InternalKey::Create(AsBytes("key"), 1, ValueKind::Value).value();
    builder.Add(key.encoded(), AsBytes("value")).value();
    builder.Finish().value();
    return builder.file_size();
}

Result<TableLookupKind> AllocationPointRead(TableCache& cache, std::uint64_t number,
                                            std::uint64_t size, std::vector<std::byte>& output) {
    const auto key = LookupKey::CreateTrusted(AsBytes("key"), 100);
    return cache.Get(number, size, key, output);
}

TEST(CacheAllocationTest, ReaderConstructionClosesHintsBeforeEveryLaterOwnershipAllocation) {
    const InternalKeyComparator comparator(BytewiseComparator());
    const auto files = TableFilesForOneShard();
    std::array<std::uint64_t, 3> sizes{};
    const auto prepare = [&](test_support::MemoryFileSystem& file_system) {
        for (std::size_t index = 0; index < files.size(); ++index) {
            sizes[index] = WriteAllocationTable(file_system, comparator, files[index]);
        }
    };
    AllocationSnapshot profiled;
    std::size_t table_allocation = 0;
    {
        test_support::MemoryFileSystem file_system;
        prepare(file_system);
        TableCache cache(file_system, "db", comparator, {}, 16, true);
        std::vector<std::byte> output;
        ASSERT_EQ(AllocationPointRead(cache, files[0], sizes[0], output), TableLookupKind::Value);
        std::optional<TableCache::Handle> retained;
        {
            AllocationSession record(profiled);
            retained.emplace(std::move(cache.Find(files[1], sizes[1])).value());
        }
        ASSERT_LE(profiled.calls, profiled.addresses.size());
        const void* table = &retained->value();
        bool found = false;
        for (std::size_t index = 0; index < profiled.calls; ++index) {
            if (profiled.addresses[index] == table) {
                found = true;
                table_allocation = index;
            }
        }
        ASSERT_TRUE(found);
        ASSERT_GE(profiled.calls, table_allocation + 4);
        EXPECT_EQ(profiled.sizes[table_allocation], sizeof(Table));
        EXPECT_EQ(profiled.sizes[profiled.calls - 2], sizeof(std::shared_ptr<const Table>));
    }

    for (std::size_t later = 1; later <= 3; ++later) {
        SCOPED_TRACE(later);
        test_support::MemoryFileSystem file_system;
        prepare(file_system);
        TableCache cache(file_system, "db", comparator, {}, 16, true);
        std::optional<Result<TableLookupKind>> read;
        std::exception_ptr error;
        std::vector<std::byte> output;
        std::latch primed(1);
        std::latch continue_read(1);
        std::latch entered(1);
        std::latch finish_read(1);
        std::jthread reader;
        ReleaseLatch continue_guard(continue_read);
        ReleaseLatch finish_guard(finish_read);
        reader = std::jthread([&] {
            ReleaseLatch primed_guard(primed);
            try {
                read.emplace(AllocationPointRead(cache, files[0], sizes[0], output));
                primed_guard.Open();
                continue_read.wait();
                read.emplace(AllocationPointRead(cache, files[0], sizes[0], output));
            } catch (...) {
                error = std::current_exception();
            }
        });
        primed.wait();
        ASSERT_FALSE(error);
        ASSERT_TRUE(read.has_value());
        ASSERT_EQ(*read, TableLookupKind::Value);
        AllocationSnapshot failed;
        bool threw = false;
        {
            // Table construction can allocate debug-library bookkeeping.
            // The final stages are its control block, holder, and cache entry.
            AllocationSession fail(failed, profiled.calls - 3 + later - 1);
            try {
                static_cast<void>(cache.Find(files[1], sizes[1]));
            } catch (const std::bad_alloc&) {
                threw = true;
            }
        }
        EXPECT_TRUE(threw);
        EXPECT_EQ(failed.calls, profiled.calls - 3 + later - 1);

        const auto operation = "read " + TableFileName("db", files[0]).filename().string();
        const auto reader_id = reader.get_id();
        file_system.SetOperationHook([&](std::string_view current) -> Status {
            if (current == operation && std::this_thread::get_id() == reader_id) {
                entered.count_down();
                finish_read.wait();
            }
            return {};
        });
        continue_guard.Open();
        entered.wait();
        std::vector<std::byte> other;
        ASSERT_EQ(AllocationPointRead(cache, files[2], sizes[2], other), TableLookupKind::Value);
        const auto retained = cache.Find(files[0], sizes[0] + 1);
        EXPECT_TRUE(retained.has_value());
        finish_guard.Open();
        reader.join();
        file_system.SetOperationHook({});
        ASSERT_FALSE(error);
        ASSERT_TRUE(read.has_value());
        EXPECT_EQ(*read, TableLookupKind::Value);
        EXPECT_EQ(AsStringView(output), "value");
    }
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
        ASSERT_TRUE(cache
                        .Insert(AsBytes(keys[index]),
                                std::make_unique<const int>(static_cast<int>(index)), 1)
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

TEST(CacheAllocationTest, OverflowRejectionDoesNotAllocatePreparedBucketGrowth) {
    constexpr std::size_t Maximum = std::numeric_limits<std::size_t>::max();
    ShardedLruCache<int> cache(Maximum);
    const auto keys = KeysForShard(0, 5);
    std::vector<ShardedLruCache<int>::Handle> originals;
    originals.reserve(4);
    for (std::size_t index = 0; index < 4; ++index) {
        auto inserted =
            cache.Insert(AsBytes(keys[index]), std::make_unique<const int>(static_cast<int>(index)),
                         index == 0 ? Maximum - 3 : 1);
        ASSERT_TRUE(inserted.has_value());
        originals.push_back(std::move(*inserted));
    }
    ASSERT_EQ(cache.total_charge(), Maximum);

    bool allocation_failed = false;
    std::optional<ShardedLruCache<int>::Handle> rejected;
    AllocationSnapshot allocations;
    {
        auto value = std::make_unique<const int>(4);
        AllocationSession session(allocations, 1);
        try {
            auto inserted = cache.Insert(AsBytes(keys[4]), std::move(value), 1);
            if (inserted.has_value()) {
                rejected.emplace(std::move(*inserted));
            }
        } catch (const std::bad_alloc&) {
            allocation_failed = true;
        }
    }

    EXPECT_FALSE(allocation_failed);
    EXPECT_EQ(allocations.calls, 1U);
    ASSERT_TRUE(rejected.has_value());
    EXPECT_EQ(**rejected, 4);
    EXPECT_FALSE(cache.Lookup(AsBytes(keys[4])).has_value());
    EXPECT_EQ(cache.total_charge(), Maximum);
    for (std::size_t index = 0; index < originals.size(); ++index) {
        auto found = cache.Lookup(AsBytes(keys[index]));
        ASSERT_TRUE(found.has_value());
        EXPECT_EQ(**found, static_cast<int>(index));
        EXPECT_EQ(*originals[index], static_cast<int>(index));
    }
    cache.Erase(AsBytes(keys[0]));
    EXPECT_EQ(cache.total_charge(), 3U);
}

TEST(CacheAllocationTest, ReplacementKeepsTheNextDistinctInsertAtTheGrowthBoundary) {
    ShardedLruCache<int> cache(16 * 16);
    const auto keys = KeysForShard(0, 5);
    std::vector<ShardedLruCache<int>::Handle> originals;
    originals.reserve(4);
    for (std::size_t index = 0; index < 4; ++index) {
        auto inserted = cache.Insert(
            AsBytes(keys[index]), std::make_unique<const int>(static_cast<int>(index)), index + 1);
        ASSERT_TRUE(inserted.has_value());
        originals.push_back(std::move(*inserted));
    }
    ASSERT_EQ(cache.total_charge(), 10U);

    std::optional<ShardedLruCache<int>::Handle> replacement;
    AllocationSnapshot replacement_allocations;
    {
        auto value = std::make_unique<const int>(20);
        AllocationSession session(replacement_allocations);
        auto inserted = cache.Insert(AsBytes(keys[1]), std::move(value), 5);
        if (inserted.has_value()) {
            replacement.emplace(std::move(*inserted));
        }
    }

    ASSERT_TRUE(replacement.has_value());
    EXPECT_EQ(replacement_allocations.calls, 1U);
    EXPECT_EQ(*originals[1], 1);
    EXPECT_EQ(**replacement, 20);
    EXPECT_EQ(cache.total_charge(), 13U);

    bool growth_failed = false;
    AllocationSnapshot growth_allocations;
    {
        auto value = std::make_unique<const int>(4);
        AllocationSession session(growth_allocations, 1);
        try {
            static_cast<void>(cache.Insert(AsBytes(keys[4]), std::move(value), 6));
        } catch (const std::bad_alloc&) {
            growth_failed = true;
        }
    }

    EXPECT_TRUE(growth_failed);
    EXPECT_EQ(growth_allocations.calls, 1U);
    EXPECT_FALSE(cache.Lookup(AsBytes(keys[4])).has_value());
    EXPECT_EQ(cache.total_charge(), 13U);
    for (std::size_t index = 0; index < originals.size(); ++index) {
        auto found = cache.Lookup(AsBytes(keys[index]));
        ASSERT_TRUE(found.has_value());
        EXPECT_EQ(**found, index == 1 ? 20 : static_cast<int>(index));
        EXPECT_EQ(*originals[index], static_cast<int>(index));
    }

    auto fifth = cache.Insert(AsBytes(keys[4]), std::make_unique<const int>(4), 6);

    ASSERT_TRUE(fifth.has_value());
    EXPECT_EQ(**fifth, 4);
    EXPECT_EQ(cache.total_charge(), 19U);
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
