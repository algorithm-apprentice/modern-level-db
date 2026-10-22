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

struct RecordingIntegerCompare {
    int operator()(std::uint64_t left, std::uint64_t right) const {
        comparisons.emplace_back(left, right);
        return IntegerCompare{}(left, right);
    }

    mutable std::vector<std::pair<std::uint64_t, std::uint64_t>> comparisons;
};

struct PreparedIntegerTarget {
    std::uint64_t value;
};

struct ConvertibleIntegerTarget {
    std::uint64_t value;
    std::size_t* conversions;

    operator std::uint64_t() const noexcept {
        ++*conversions;
        return value;
    }
};

struct LvalueIntegerTarget {
    std::uint64_t value;
    std::size_t* conversions;

    operator std::uint64_t() & noexcept {
        ++*conversions;
        return value;
    }
};

struct RvalueIntegerTarget {
    std::uint64_t value;
    std::size_t* conversions;

    operator std::uint64_t() && noexcept {
        ++*conversions;
        return value;
    }
};

struct PreparedIntegerCompare {
    int operator()(std::uint64_t left, std::uint64_t right) const noexcept {
        return IntegerCompare{}(left, right);
    }

    int operator()(std::uint64_t left, const PreparedIntegerTarget& right) const noexcept {
        ++prepared_comparisons;
        return IntegerCompare{}(left, right.value);
    }

    mutable std::size_t prepared_comparisons = 0;
};

struct DecadeCompare {
    int operator()(std::uint64_t left, std::uint64_t right) const noexcept {
        return IntegerCompare{}(left / 10U, right / 10U);
    }
};

struct ByteViewCompare {
    int operator()(ByteView left, ByteView right) const noexcept {
        return BytewiseComparator().Compare(left, right);
    }
};

struct alignas(64) OverAlignedKey {
    std::uint64_t value;
    std::array<std::byte, 64 - sizeof(std::uint64_t)> padding;
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

template <typename Entry, typename Compare>
concept HasSkipList = requires { typename SkipList<Entry, Compare>; };

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

TEST(SkipListTest, InsertsFindsAndIteratesUniqueKeys) {
    Arena arena;
    IntegerCompare compare;
    IntegerList list(compare, arena);

    list.Insert(10);
    list.Insert(5);
    list.Insert(20);
    EXPECT_TRUE(list.Contains(5));
    EXPECT_TRUE(list.Contains(10));
    EXPECT_TRUE(list.Contains(20));
    EXPECT_FALSE(list.Contains(15));

    IntegerList::Iterator iterator(list);
    iterator.SeekToFirst();
    for (const std::uint64_t expected : std::array<std::uint64_t, 3>{5U, 10U, 20U}) {
        ASSERT_TRUE(iterator.valid());
        EXPECT_EQ(iterator.entry(), expected);
        iterator.Next();
    }
    EXPECT_FALSE(iterator.valid());
}

TEST(SkipListTest, CompactNodesMatchLevelDbArenaConsumption) {
    Arena arena;
    IntegerCompare compare;
    IntegerList list(compare, arena);
    constexpr std::size_t block_charge = 4U * 1'024U + sizeof(std::byte*);

    EXPECT_EQ(arena.memory_usage(), block_charge);
    for (std::uint64_t key = 1; key <= 216U; ++key) {
        list.Insert(key);
    }
    EXPECT_EQ(arena.memory_usage(), block_charge);

    list.Insert(217U);
    EXPECT_EQ(arena.memory_usage(), 2U * block_charge);
}

TEST(SkipListTest, IteratesSeeksAndMovesBackward) {
    Arena arena;
    IntegerCompare compare;
    IntegerList list(compare, arena);
    for (const std::uint64_t key : std::array<std::uint64_t, 4>{30, 10, 40, 20}) {
        list.Insert(key);
    }

    IntegerList::Iterator iterator(list);
    iterator.SeekToFirst();
    for (const std::uint64_t expected : std::array<std::uint64_t, 4>{10, 20, 30, 40}) {
        ASSERT_TRUE(iterator.valid());
        EXPECT_EQ(iterator.entry(), expected);
        iterator.Next();
    }
    EXPECT_FALSE(iterator.valid());

    iterator.Seek(25);
    ASSERT_TRUE(iterator.valid());
    EXPECT_EQ(iterator.entry(), 30U);
    iterator.Seek(40);
    ASSERT_TRUE(iterator.valid());
    EXPECT_EQ(iterator.entry(), 40U);
    iterator.Seek(41);
    EXPECT_FALSE(iterator.valid());

    iterator.SeekToLast();
    for (const std::uint64_t expected : std::array<std::uint64_t, 4>{40, 30, 20, 10}) {
        ASSERT_TRUE(iterator.valid());
        EXPECT_EQ(iterator.entry(), expected);
        iterator.Prev();
    }
    EXPECT_FALSE(iterator.valid());
}

TEST(SkipListTest, LastPositioningAvoidsComparisonsAndPrevUsesStrictBounds) {
    Arena arena;
    RecordingIntegerCompare compare;
    SkipList<std::uint64_t, RecordingIntegerCompare> list(compare, arena);
    list.Insert(10);
    list.Insert(20);
    list.Insert(30);
    compare.comparisons.clear();

    SkipList<std::uint64_t, RecordingIntegerCompare>::Iterator iterator(list);
    iterator.SeekToLast();

    ASSERT_TRUE(iterator.valid());
    EXPECT_EQ(iterator.entry(), 30U);
    EXPECT_TRUE(compare.comparisons.empty());

    iterator.Prev();

    ASSERT_TRUE(iterator.valid());
    EXPECT_EQ(iterator.entry(), 20U);
    ASSERT_FALSE(compare.comparisons.empty());
    bool saw_equal_bound = false;
    for (const auto& [left, right] : compare.comparisons) {
        EXPECT_EQ(right, 30U);
        saw_equal_bound = saw_equal_bound || left == right;
    }
    EXPECT_TRUE(saw_equal_bound);

    compare.comparisons.clear();
    iterator.SeekToFirst();
    ASSERT_TRUE(iterator.valid());
    EXPECT_EQ(iterator.entry(), 10U);
    EXPECT_TRUE(compare.comparisons.empty());

    iterator.Prev();

    EXPECT_FALSE(iterator.valid());
    ASSERT_FALSE(compare.comparisons.empty());
    saw_equal_bound = false;
    for (const auto& [left, right] : compare.comparisons) {
        EXPECT_EQ(right, 10U);
        saw_equal_bound = saw_equal_bound || left == right;
    }
    EXPECT_TRUE(saw_equal_bound);
}

TEST(SkipListTest, ConvertsEntryCompatibleTargetsOnceBeforeSearching) {
    Arena arena;
    IntegerCompare compare;
    IntegerList list(compare, arena);
    IntegerList::Iterator iterator(list);
    std::size_t conversions = 0;
    iterator.Seek(ConvertibleIntegerTarget{10, &conversions});
    EXPECT_EQ(conversions, 1U);
    EXPECT_FALSE(iterator.valid());

    for (std::uint64_t key = 2; key <= 512; key += 2) {
        list.Insert(key);
    }
    for (const std::uint64_t target : {0U, 255U, 256U, 511U, 512U, 513U}) {
        SCOPED_TRACE(target);
        conversions = 0;
        iterator.Seek(ConvertibleIntegerTarget{target, &conversions});
        EXPECT_EQ(conversions, 1U);
        ASSERT_EQ(iterator.valid(), target <= 512);
        if (iterator.valid()) {
            const std::uint64_t expected = target < 2 ? 2 : ((target + 1) / 2) * 2;
            EXPECT_EQ(iterator.entry(), expected);
        }
    }
}

TEST(SkipListTest, PreservesReferenceQualifiedEntryConversions) {
    Arena arena;
    IntegerCompare compare;
    IntegerList list(compare, arena);
    IntegerList::Iterator iterator(list);
    std::size_t conversions = 0;
    LvalueIntegerTarget target{20, &conversions};
    iterator.Seek(target);
    EXPECT_EQ(conversions, 1U);
    EXPECT_FALSE(iterator.valid());

    list.Insert(10);
    list.Insert(30);
    conversions = 0;
    iterator.Seek(target);
    EXPECT_EQ(conversions, 1U);
    ASSERT_TRUE(iterator.valid());
    EXPECT_EQ(iterator.entry(), 30U);

    conversions = 0;
    iterator.Seek(RvalueIntegerTarget{10, &conversions});
    EXPECT_EQ(conversions, 1U);
    ASSERT_TRUE(iterator.valid());
    EXPECT_EQ(iterator.entry(), 10U);
}

TEST(SkipListTest, SeeksWithPreparedTargetsWithoutConvertingThemToStoredEntries) {
    Arena arena;
    PreparedIntegerCompare compare;
    SkipList<std::uint64_t, PreparedIntegerCompare> list(compare, arena);
    SkipList<std::uint64_t, PreparedIntegerCompare>::Iterator iterator(list);
    iterator.Seek(PreparedIntegerTarget{20});
    EXPECT_FALSE(iterator.valid());
    EXPECT_EQ(compare.prepared_comparisons, 0U);

    list.Insert(10);
    list.Insert(30);
    list.Insert(50);
    for (const std::uint64_t target : {0U, 10U, 11U, 30U, 31U, 50U, 51U}) {
        SCOPED_TRACE(target);
        compare.prepared_comparisons = 0;
        iterator.Seek(PreparedIntegerTarget{target});
        EXPECT_GT(compare.prepared_comparisons, 0U);
        ASSERT_EQ(iterator.valid(), target <= 50);
        if (iterator.valid()) {
            const std::uint64_t expected = target <= 10 ? 10 : (target <= 30 ? 30 : 50);
            EXPECT_EQ(iterator.entry(), expected);
        }
    }

    list.Insert(20);
    iterator.Seek(PreparedIntegerTarget{20});
    ASSERT_TRUE(iterator.valid());
    EXPECT_EQ(iterator.entry(), 20U);
    EXPECT_TRUE(list.Contains(30));
}

TEST(SkipListTest, PreparedAndStoredTargetsAgreeAcrossMultipleLevels) {
    Arena arena;
    PreparedIntegerCompare compare;
    SkipList<std::uint64_t, PreparedIntegerCompare> list(compare, arena);
    std::set<std::uint64_t> model;
    for (std::uint64_t key = 2; key <= 512; key += 2) {
        list.Insert(key);
        model.insert(key);
    }

    SkipList<std::uint64_t, PreparedIntegerCompare>::Iterator iterator(list);
    for (std::uint64_t target = 0; target <= 513; ++target) {
        SCOPED_TRACE(target);
        EXPECT_EQ(list.Contains(target), model.contains(target));
        iterator.Seek(PreparedIntegerTarget{target});
        const auto expected = model.lower_bound(target);
        ASSERT_EQ(iterator.valid(), expected != model.end());
        if (iterator.valid()) {
            EXPECT_EQ(iterator.entry(), *expected);
        }
    }
}

TEST(SkipListTest, LowerBoundComparesEachVisitedNodeAtMostOnce) {
    Arena arena;
    RecordingIntegerCompare compare;
    SkipList<std::uint64_t, RecordingIntegerCompare> list(compare, arena);
    std::set<std::uint64_t> model;
    for (std::uint64_t key = 2; key <= 512; key += 2) {
        list.Insert(key);
        model.insert(key);
    }

    SkipList<std::uint64_t, RecordingIntegerCompare>::Iterator iterator(list);
    for (std::uint64_t target = 0; target <= 513; ++target) {
        SCOPED_TRACE(target);
        compare.comparisons.clear();
        iterator.Seek(target);
        const auto expected = model.lower_bound(target);
        ASSERT_EQ(iterator.valid(), expected != model.end());
        if (iterator.valid()) {
            EXPECT_EQ(iterator.entry(), *expected);
        }
        std::set<std::uint64_t> visited;
        for (const auto& [left, right] : compare.comparisons) {
            EXPECT_EQ(right, target);
            ASSERT_TRUE(visited.insert(left).second) << "repeated node " << left;
        }
    }
}

TEST(SkipListTest, IteratorsObserveLaterNeighborsWithoutLosingCurrentNodes) {
    Arena arena;
    IntegerCompare compare;
    IntegerList list(compare, arena);
    list.Insert(10);
    list.Insert(30);

    IntegerList::Iterator forward(list);
    forward.Seek(10);
    ASSERT_TRUE(forward.valid());
    EXPECT_EQ(forward.entry(), 10U);

    list.Insert(20);

    EXPECT_EQ(forward.entry(), 10U);
    forward.Next();
    ASSERT_TRUE(forward.valid());
    EXPECT_EQ(forward.entry(), 20U);

    list.Insert(40);

    EXPECT_EQ(forward.entry(), 20U);
    forward.Next();
    ASSERT_TRUE(forward.valid());
    EXPECT_EQ(forward.entry(), 30U);
    forward.Next();
    ASSERT_TRUE(forward.valid());
    EXPECT_EQ(forward.entry(), 40U);

    IntegerList::Iterator backward(list);
    backward.Seek(30);
    ASSERT_TRUE(backward.valid());
    EXPECT_EQ(backward.entry(), 30U);

    list.Insert(25);

    EXPECT_EQ(backward.entry(), 30U);
    backward.Prev();
    ASSERT_TRUE(backward.valid());
    EXPECT_EQ(backward.entry(), 25U);
}

TEST(SkipListTest, ComparatorEquivalenceDefinesContainsAndLowerBounds) {
    Arena arena;
    DecadeCompare compare;
    SkipList<std::uint64_t, DecadeCompare> list(compare, arena);
    list.Insert(10);
    list.Insert(30);
    list.Insert(50);

    EXPECT_TRUE(list.Contains(19));
    EXPECT_FALSE(list.Contains(20));
    EXPECT_TRUE(list.Contains(39));
    EXPECT_FALSE(list.Contains(49));

    SkipList<std::uint64_t, DecadeCompare>::Iterator iterator(list);
    iterator.Seek(19);
    ASSERT_TRUE(iterator.valid());
    EXPECT_EQ(iterator.entry(), 10U);
    iterator.Seek(20);
    ASSERT_TRUE(iterator.valid());
    EXPECT_EQ(iterator.entry(), 30U);
    iterator.Seek(49);
    ASSERT_TRUE(iterator.valid());
    EXPECT_EQ(iterator.entry(), 50U);
    iterator.Seek(60);
    EXPECT_FALSE(iterator.valid());
}

TEST(SkipListTest, AlignsNodesAfterAnUnalignedArenaAllocation) {
    Arena arena;
    std::byte* prefix = arena.Allocate(1);
    *prefix = std::byte{0xa5};
    const std::size_t initial_usage = arena.memory_usage();
    IntegerCompare compare;
    IntegerList list(compare, arena);
    for (const std::uint64_t key : std::array<std::uint64_t, 7>{70, 10, 50, 20, 60, 30, 40}) {
        list.Insert(key);
    }

    EXPECT_EQ(*prefix, std::byte{0xa5});
    EXPECT_EQ(arena.memory_usage(), initial_usage);
    IntegerList::Iterator iterator(list);
    iterator.SeekToFirst();
    for (std::uint64_t expected = 10; expected <= 70; expected += 10) {
        ASSERT_TRUE(iterator.valid());
        EXPECT_EQ(iterator.entry(), expected);
        iterator.Next();
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
        if (model.insert(key).second) {
            list.Insert(key);
        }
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
            EXPECT_EQ(iterator.entry(), *expected);
        }
    }

    IntegerList::Iterator iterator(list);
    iterator.SeekToFirst();
    for (const std::uint64_t expected : model) {
        ASSERT_TRUE(iterator.valid());
        EXPECT_EQ(iterator.entry(), expected);
        iterator.Next();
    }
    EXPECT_FALSE(iterator.valid());
}

TEST(SkipListTest, StoresArenaBackedBinaryViews) {
    Arena arena;
    ByteViewCompare compare;
    SkipList<ByteView, ByteViewCompare> list(compare, arena);

    auto allocate_key = [&](std::string_view text) {
        MutableByteView storage(arena.Allocate(text.size()), text.size());
        std::ranges::copy(AsBytes(text), storage.begin());
        return ByteView(storage);
    };

    const ByteView alpha = allocate_key(std::string_view("a\0x", 3));
    const ByteView between = allocate_key(std::string_view("a\0y", 3));
    const ByteView beta = allocate_key(std::string_view("b\0y", 3));
    const ByteView after = allocate_key("z");
    list.Insert(beta);
    list.Insert(alpha);
    EXPECT_TRUE(list.Contains(beta));
    EXPECT_FALSE(list.Contains(between));
    EXPECT_FALSE(list.Contains(after));

    SkipList<ByteView, ByteViewCompare>::Iterator iterator(list);
    iterator.SeekToFirst();
    ASSERT_TRUE(iterator.valid());
    EXPECT_TRUE(std::ranges::equal(iterator.entry(), alpha));
    iterator.Next();
    ASSERT_TRUE(iterator.valid());
    EXPECT_TRUE(std::ranges::equal(iterator.entry(), beta));
}

TEST(SkipListTest, ConcurrentReadersObserveOnlyInitializedOrderedNodes) {
    constexpr std::uint64_t InsertedCount = 4'096;
    constexpr std::uint64_t Modulus = 4'099;
    constexpr int ReaderCount = 4;

    Arena arena;
    ConcurrentKeyCompare compare;
    SkipList<ConcurrentKey, ConcurrentKeyCompare> list(compare, arena);
    list.Insert(MakeConcurrentKey(0));
    list.Insert(MakeConcurrentKey(Modulus));

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
                    const ConcurrentKey& key = iterator.entry();
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
        list.Insert(MakeConcurrentKey(order));
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
        const ConcurrentKey& key = iterator.entry();
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

    std::uint64_t missing = 1;
    while (missing < Modulus && list.Contains(MakeConcurrentKey(missing))) {
        ++missing;
    }
    ASSERT_LT(missing, Modulus);
    EXPECT_FALSE(list.Contains(MakeConcurrentKey(missing)));
    EXPECT_FALSE(list.Contains(MakeConcurrentKey(Modulus + 1U)));
    EXPECT_GT(arena.memory_usage(), 0U);
}

}  // namespace
}  // namespace modern_leveldb
