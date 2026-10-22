#include "table/clock_block_cache.h"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "modern_leveldb/base/coding.h"
#include "modern_leveldb/base/comparator.h"
#include "modern_leveldb/base/hash.h"
#include "table/block.h"
#include "table/block_builder.h"
#include "table/clock_block_cache_protocol.h"

namespace modern_leveldb {

class ClockBlockCacheTestPeer {
public:
    using Root = clock_cache_detail::LookupRoot;
    using Slot = clock_cache_detail::Slot;
    using Shard = clock_cache_detail::Shard;

    class PendingRelease {
    public:
        PendingRelease(Slot& slot, Shard& shard) noexcept : slot_(&slot), shard_(&shard) {}
        PendingRelease(const PendingRelease&) = delete;
        PendingRelease& operator=(const PendingRelease&) = delete;
        PendingRelease(PendingRelease&& source) noexcept
            : slot_(std::exchange(source.slot_, nullptr)), shard_(source.shard_) {}
        ~PendingRelease() { Finish(); }

        void Finish() noexcept {
            if (auto* slot = std::exchange(slot_, nullptr)) {
                clock_cache_detail::FinishRelease(*slot, *shard_);
            }
        }

    private:
        Slot* slot_;
        Shard* shard_;
    };

    static Root AcquireRoot(ClockBlockCache& cache, ClockBlockCache::Key key) {
        return cache.AcquireLookupRoot(key);
    }
    static std::optional<ClockBlockCache::Handle> LookupInRoot(const Root& root,
                                                               ClockBlockCache::Key key) {
        return ClockBlockCache::LookupInRoot(root, key);
    }
    static Slot* Position(const ClockBlockCache::Handle& handle) { return handle.slot_; }
    static std::optional<ClockBlockCache::Handle> PinObserved(Slot& slot, std::uint64_t& observed) {
        if ((observed & clock_cache_detail::StateMask) != clock_cache_detail::Live ||
            !clock_cache_detail::PinOnce(slot, observed)) {
            return std::nullopt;
        }
        return ClockBlockCache::Handle(slot);
    }
    static std::optional<PendingRelease> ReleaseToPending(ClockBlockCache::Handle& handle) {
        auto* slot = std::exchange(handle.slot_, nullptr);
        auto* shard = clock_cache_detail::DropPin(*slot);
        if (shard == nullptr) {
            return std::nullopt;
        }
        return PendingRelease(*slot, *shard);
    }
};

namespace {

using Key = std::array<std::byte, 16>;

Key KeyOf(std::uint64_t identity, std::uint64_t offset) {
    Key key{};
    EncodeFixed64(MutableByteView(key).first<8>(), identity);
    EncodeFixed64(MutableByteView(key).last<8>(), offset);
    return key;
}

std::unique_ptr<const Block> ValueOf(std::string_view value) {
    BlockBuilder builder(16);
    builder.Add(AsBytes("key"), AsBytes(value));
    const auto bytes = builder.Finish();
    auto block = Block::Create(std::vector<std::byte>(bytes.begin(), bytes.end())).value();
    return std::make_unique<const Block>(std::move(block));
}

std::string TextOf(const ClockBlockCache::Handle& handle) {
    Block::Iterator cursor(handle.value(), BytewiseComparator());
    cursor.SeekToFirst().value();
    return std::string(AsStringView(cursor.value()));
}

Key CollidingKey(std::uint32_t number, std::uint32_t target = 0x01234567U) {
    constexpr std::uint32_t Multiplier = 0xc6a4a793U;
    constexpr std::uint32_t Inverse = 0xd8198e9bU;
    const std::array<std::uint32_t, 3> words{number, 0x5a5a1234U, number * 0x9e3779b9U};
    auto hash = std::uint32_t{16} * Multiplier;
    Key key{};
    for (std::size_t index = 0; index < words.size(); ++index) {
        EncodeFixed32(MutableByteView(key).subspan(index * 4).first<4>(), words[index]);
        hash = (hash + words[index]) * Multiplier;
        hash ^= hash >> 16U;
    }
    const auto final_word = ((target ^ (target >> 16U)) * Inverse) - hash;
    EncodeFixed32(MutableByteView(key).last<4>(), final_word);
    return key;
}

static_assert(!std::is_copy_constructible_v<ClockBlockCache::Handle>);
static_assert(std::is_nothrow_move_constructible_v<ClockBlockCache::Handle>);
static_assert(std::is_nothrow_move_assignable_v<ClockBlockCache::Handle>);

TEST(ClockBlockCacheTest, LookupRetainsTheExactImmutableBlockWithoutTransferringOwnership) {
    ClockBlockCache cache(16 * 4096);
    const auto key = KeyOf(11, 37);
    auto inserted = cache.Insert(key, ValueOf("value"), 4096);
    ASSERT_TRUE(inserted.has_value());
    EXPECT_EQ(TextOf(*inserted), "value");
    const auto found = cache.Lookup(key);
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(&found->value(), &inserted->value());
    EXPECT_EQ(TextOf(*found), "value");
    EXPECT_EQ(cache.total_charge(), 4096U);
    EXPECT_FALSE(cache.Lookup(KeyOf(11, 38)).has_value());
    EXPECT_FALSE(cache.Lookup(KeyOf(12, 37)).has_value());
}

TEST(ClockBlockCacheTest, EraseAndReplacementKeepEveryOldLeaseAlive) {
    ClockBlockCache cache(16 * 4096);
    const auto key = KeyOf(1, 9);
    auto old = cache.Insert(key, ValueOf("old"), 4096);
    ASSERT_TRUE(old.has_value());
    auto replacement = cache.Insert(key, ValueOf("new"), 4096);
    ASSERT_TRUE(replacement.has_value());
    EXPECT_EQ(TextOf(*old), "old");
    EXPECT_EQ(TextOf(*replacement), "new");
    const auto found = cache.Lookup(key);
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(TextOf(*found), "new");
    EXPECT_EQ(cache.total_charge(), 4096U);
    cache.Erase(key);
    EXPECT_FALSE(cache.Lookup(key).has_value());
    EXPECT_EQ(cache.total_charge(), 0U);
    EXPECT_EQ(TextOf(*old), "old");
    EXPECT_EQ(TextOf(*replacement), "new");
    EXPECT_EQ(TextOf(*found), "new");
}

TEST(ClockBlockCacheTest, ZeroCapacityReturnsAnUncachedLeaseAndReleasesExternalAccounting) {
    ClockBlockCache cache(0);
    const auto key = KeyOf(7, 8);
    {
        auto inserted = cache.Insert(key, ValueOf("uncached"), 4096);
        ASSERT_TRUE(inserted.has_value());
        EXPECT_EQ(TextOf(*inserted), "uncached");
        EXPECT_FALSE(cache.Lookup(key).has_value());
        EXPECT_EQ(cache.total_charge(), 0U);
        EXPECT_GT(cache.GetStats().standalone_bytes, 0U);
    }
    EXPECT_EQ(cache.GetStats().standalone_bytes, 0U);
    EXPECT_GT(cache.GetStats().peak_standalone_bytes, 0U);
}

TEST(ClockBlockCacheTest, UncachedReplacementInvalidatesTheOldMappingButNotItsLease) {
    ClockBlockCache cache(16 * 4096);
    const auto key = KeyOf(3, 4);
    auto old = cache.Insert(key, ValueOf("old"), 4096);
    ASSERT_TRUE(old.has_value());
    auto oversized =
        cache.Insert(key, ValueOf("standalone"), std::numeric_limits<std::size_t>::max());
    ASSERT_TRUE(oversized.has_value());
    EXPECT_EQ(TextOf(*old), "old");
    EXPECT_EQ(TextOf(*oversized), "standalone");
    EXPECT_FALSE(cache.Lookup(key).has_value());
    EXPECT_EQ(cache.total_charge(), 0U);
}

TEST(ClockBlockCacheTest, RejectsInvalidOwnersAndChargesWithoutChangingAnExistingMapping) {
    ClockBlockCache cache(16 * 4096);
    const auto key = KeyOf(9, 10);
    ASSERT_TRUE(cache.Insert(key, ValueOf("old"), 4096).has_value());
    const auto null = cache.Insert(key, nullptr, 4096);
    ASSERT_FALSE(null.has_value());
    EXPECT_EQ(null.error().code(), ErrorCode::InvalidArgument);
    const auto free = cache.Insert(key, ValueOf("wrong"), 0);
    ASSERT_FALSE(free.has_value());
    EXPECT_EQ(free.error().code(), ErrorCode::InvalidArgument);
    const auto found = cache.Lookup(key);
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(TextOf(*found), "old");
    EXPECT_EQ(cache.total_charge(), 4096U);
}

TEST(ClockBlockCacheTest, FullKeysRemainReachableAcrossExactHashClustersAndIndexGrowth) {
    ClockBlockCache cache(16 * 4096 * 128);
    for (std::uint32_t index = 0; index < 96; ++index) {
        const auto key = CollidingKey(index);
        ASSERT_EQ(Hash32(key, 0), 0x01234567U);
        ASSERT_TRUE(cache.Insert(key, ValueOf(std::to_string(index)), 4096).has_value());
    }
    EXPECT_EQ(cache.total_charge(), 96U * 4096U);
    for (std::uint32_t index = 0; index < 96; ++index) {
        const auto found = cache.Lookup(CollidingKey(index));
        ASSERT_TRUE(found.has_value()) << index;
        EXPECT_EQ(TextOf(*found), std::to_string(index));
    }
    const auto stats = cache.GetStats();
    EXPECT_GT(stats.retained_index_bytes, 0U);
    EXPECT_LE(stats.metadata_bytes, stats.metadata_allowance);
}

TEST(ClockBlockCacheTest, RefreshAndPositionReuseDoNotAccumulateTombstoneSnapshots) {
    ClockBlockCache cache(16 * 4096 * 8);
    auto key = CollidingKey(0, 0x01234560U);
    ASSERT_TRUE(cache.Insert(key, ValueOf("value"), 4096).has_value());
    const auto initial = cache.GetStats();
    for (std::uint32_t index = 1; index <= 1024; ++index) {
        cache.Erase(key);
        key = CollidingKey(index, 0x01234560U | (index & 7U));
        ASSERT_TRUE(cache.Insert(key, ValueOf("value"), 4096).has_value());
        ASSERT_TRUE(cache.Lookup(key).has_value());
    }
    const auto after = cache.GetStats();
    EXPECT_EQ(after.metadata_bytes, initial.metadata_bytes);
    EXPECT_EQ(after.retained_index_bytes, initial.retained_index_bytes);
    EXPECT_GT(after.index_refreshes, 0U);
    EXPECT_LE(after.metadata_bytes, after.metadata_allowance);
    EXPECT_EQ(cache.total_charge(), 4096U);
}

TEST(ClockBlockCacheTest, ActualMetadataAllowanceBoundsTinyChargedAdmissions) {
    ClockBlockCache cache(16 * 4096);
    for (std::uint32_t index = 0; index < 1024; ++index) {
        auto inserted = cache.Insert(CollidingKey(index), ValueOf("tiny"), 1);
        ASSERT_TRUE(inserted.has_value());
        EXPECT_EQ(TextOf(*inserted), "tiny");
    }
    const auto stats = cache.GetStats();
    EXPECT_GT(stats.admission_rejections, 0U);
    EXPECT_LE(stats.metadata_bytes, stats.metadata_allowance);
    EXPECT_LT(cache.total_charge(), 4096U);
    EXPECT_EQ(stats.standalone_bytes, 0U);
}

TEST(ClockBlockCacheTest, AggregateChargeSaturatesAcrossRoundedShardCapacities) {
    constexpr auto Maximum = std::numeric_limits<std::size_t>::max();
    ClockBlockCache cache(Maximum);
    constexpr auto Charge = Maximum / 16 + 1;
    for (std::uint32_t shard = 0; shard < 16; ++shard) {
        const auto key = CollidingKey(shard, (shard << 28U) | 0x01234567U);
        auto inserted = cache.Insert(key, ValueOf("charged"), Charge);
        ASSERT_TRUE(inserted.has_value());
        EXPECT_TRUE(cache.Lookup(key).has_value());
    }
    EXPECT_EQ(cache.total_charge(), Maximum);
}

TEST(ClockBlockCacheTest, MoveAndAllPinnedPressurePreserveOwnershipWithoutUnboundedRetention) {
    ClockBlockCache cache(16 * 4096);
    auto first = cache.Insert(CollidingKey(1), ValueOf("first"), 4096);
    ASSERT_TRUE(first.has_value());
    ClockBlockCache::Handle moved(std::move(*first));
    auto second = cache.Insert(CollidingKey(2), ValueOf("second"), 4096);
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(TextOf(moved), "first");
    EXPECT_EQ(TextOf(*second), "second");
    EXPECT_EQ(cache.total_charge(), 4096U);
    EXPECT_FALSE(cache.Lookup(CollidingKey(2)).has_value());
    const auto stats = cache.GetStats();
    EXPECT_GT(stats.admission_rejections, 0U);
    EXPECT_LE(stats.metadata_bytes, stats.metadata_allowance);
}

TEST(ClockBlockCacheTest, MixedChargesErasesAndOldLeasesMatchAnIndependentQuiescentModel) {
    ClockBlockCache cache(16 * 4096 * 4);
    constexpr std::size_t Keys = 32;
    std::array<std::string, Keys> latest;
    std::array<std::size_t, Keys> charges{};
    std::array<bool, Keys> erased{};
    struct Retention {
        ClockBlockCache::Handle lease;
        std::string value;
    };
    std::vector<Retention> retained;
    std::uint32_t random = 301;
    for (std::uint32_t step = 0; step < 800; ++step) {
        random = random * 1664525U + 1013904223U;
        const auto position = (random >> 8U) % Keys;
        const auto key = CollidingKey(static_cast<std::uint32_t>(position));
        if ((random & 3U) == 0) {
            cache.Erase(key);
            erased[position] = true;
        } else {
            latest[position] = std::to_string(position) + "-" + std::to_string(step);
            erased[position] = false;
            charges[position] = 1 + ((random >> 16U) % 4096);
            auto inserted = cache.Insert(key, ValueOf(latest[position]), charges[position]);
            ASSERT_TRUE(inserted.has_value());
            EXPECT_EQ(TextOf(*inserted), latest[position]);
            if ((random & 7U) == 1) {
                retained.push_back({std::move(*inserted), latest[position]});
            }
        }
        if (retained.size() > 6) {
            retained.erase(retained.begin());
        }
        for (const auto& held : retained) {
            EXPECT_EQ(TextOf(held.lease), held.value);
        }
        std::size_t indexed = 0;
        std::size_t charge = 0;
        for (std::size_t query = 0; query < Keys; ++query) {
            const auto found = cache.Lookup(CollidingKey(static_cast<std::uint32_t>(query)));
            if (found.has_value()) {
                EXPECT_FALSE(erased[query]);
                EXPECT_EQ(TextOf(*found), latest[query]);
                ++indexed;
                charge += charges[query];
            }
        }
        const auto stats = cache.GetStats();
        EXPECT_EQ(stats.indexed_entries, indexed);
        EXPECT_EQ(cache.total_charge(), charge);
        EXPECT_LE(stats.metadata_bytes, stats.metadata_allowance);
    }
    retained.clear();
    EXPECT_EQ(cache.GetStats().standalone_bytes, 0U);
    EXPECT_EQ(cache.GetStats().erased_bytes, 0U);
}

TEST(ClockBlockCacheTest, ConcurrentInsertLookupEraseNeverReturnsAnotherFullKeysValue) {
    ClockBlockCache cache(16 * 4096 * 16);
    std::array<std::exception_ptr, 4> exceptions{};
    std::atomic<int> failures = 0;
    std::vector<std::jthread> workers;
    for (std::uint32_t worker = 0; worker < exceptions.size(); ++worker) {
        workers.emplace_back([&, worker] {
            try {
                auto random = worker + 301;
                std::optional<ClockBlockCache::Handle> retained;
                std::string retained_value;
                for (std::uint32_t step = 0; step < 8000; ++step) {
                    random = random * 1664525U + 1013904223U;
                    const auto position = (random >> 8U) % 32U;
                    const auto key = CollidingKey(position);
                    const auto expected = "value-" + std::to_string(position);
                    if ((random & 3U) == 0) {
                        auto inserted = cache.Insert(key, ValueOf(expected), 512);
                        if (!inserted.has_value() || TextOf(*inserted) != expected) {
                            ++failures;
                        }
                    } else if ((random & 3U) == 1) {
                        cache.Erase(key);
                    } else {
                        auto found = cache.Lookup(key);
                        if (found.has_value()) {
                            if (TextOf(*found) != expected) {
                                ++failures;
                            }
                            retained = std::move(found);
                            retained_value = expected;
                        }
                    }
                    if (retained.has_value() && TextOf(*retained) != retained_value) {
                        ++failures;
                    }
                }
            } catch (...) {
                exceptions[worker] = std::current_exception();
            }
        });
    }
    workers.clear();
    for (const auto& exception : exceptions) {
        EXPECT_FALSE(exception);
    }
    EXPECT_EQ(failures.load(), 0);
    EXPECT_EQ(cache.GetStats().standalone_bytes, 0U);
    EXPECT_EQ(cache.GetStats().erased_bytes, 0U);
    EXPECT_LE(cache.GetStats().metadata_bytes, cache.GetStats().metadata_allowance);
}

TEST(ClockBlockCacheProtocolTest, StaleLiveWordAcrossEvictionAndReuseStillChecksTheCompleteKey) {
    ClockBlockCache cache(16 * 4096);
    const auto old_key = CollidingKey(1);
    const auto victim_replacement = CollidingKey(2);
    const auto new_key = CollidingKey(3);
    clock_cache_detail::Slot* slot = nullptr;
    {
        auto old = cache.Insert(old_key, ValueOf("old"), 4096);
        ASSERT_TRUE(old.has_value());
        slot = ClockBlockCacheTestPeer::Position(*old);
    }
    auto observed = slot->metadata.load(std::memory_order_acquire);
    ASSERT_EQ(observed & clock_cache_detail::StateMask, clock_cache_detail::Live);
    ASSERT_TRUE(cache.Insert(victim_replacement, ValueOf("victim"), 4096).has_value());
    ASSERT_FALSE(cache.Lookup(old_key).has_value());
    {
        auto reused = cache.Insert(new_key, ValueOf("new"), 4096);
        ASSERT_TRUE(reused.has_value());
        ASSERT_EQ(ClockBlockCacheTestPeer::Position(*reused), slot);
    }
    ASSERT_EQ(slot->metadata.load(std::memory_order_acquire), observed);
    auto pinned = ClockBlockCacheTestPeer::PinObserved(*slot, observed);
    ASSERT_TRUE(pinned.has_value());
    EXPECT_EQ(TextOf(*pinned), "new");
    ASSERT_EQ(Hash32(old_key, 0), Hash32(new_key, 0));
    EXPECT_FALSE(clock_cache_detail::MatchesPinned(*slot, Hash32(old_key, 0), old_key));
    EXPECT_TRUE(clock_cache_detail::MatchesPinned(*slot, Hash32(new_key, 0), new_key));
    EXPECT_FALSE(cache.Lookup(old_key).has_value());
}

TEST(ClockBlockCacheProtocolTest, FailedStrongCasUpdatesTheObservationBeforeRetrying) {
    ClockBlockCache cache(16 * 4096);
    const auto key = CollidingKey(1);
    clock_cache_detail::Slot* slot = nullptr;
    {
        auto inserted = cache.Insert(key, ValueOf("value"), 4096);
        ASSERT_TRUE(inserted.has_value());
        slot = ClockBlockCacheTestPeer::Position(*inserted);
    }
    auto observed = slot->metadata.load(std::memory_order_acquire);
    auto competing = cache.Lookup(key);
    ASSERT_TRUE(competing.has_value());
    const auto missed = ClockBlockCacheTestPeer::PinObserved(*slot, observed);
    ASSERT_FALSE(missed.has_value());
    EXPECT_EQ(observed, slot->metadata.load(std::memory_order_acquire));
    auto retried = ClockBlockCacheTestPeer::PinObserved(*slot, observed);
    ASSERT_TRUE(retried.has_value());
    EXPECT_EQ(TextOf(*retried), "value");
}

TEST(ClockBlockCacheProtocolTest, AcquiredPinKeepsItsGenerationAcrossEraseAndReplacement) {
    ClockBlockCache cache(16 * 4096 * 8);
    const auto key = CollidingKey(1);
    clock_cache_detail::Slot* slot = nullptr;
    {
        auto old = cache.Insert(key, ValueOf("old"), 4096);
        ASSERT_TRUE(old.has_value());
        slot = ClockBlockCacheTestPeer::Position(*old);
    }
    auto observed = slot->metadata.load(std::memory_order_acquire);
    auto pinned = ClockBlockCacheTestPeer::PinObserved(*slot, observed);
    ASSERT_TRUE(pinned.has_value());
    cache.Erase(key);
    auto replacement = cache.Insert(key, ValueOf("new"), 4096);
    ASSERT_TRUE(replacement.has_value());
    EXPECT_NE(ClockBlockCacheTestPeer::Position(*replacement), slot);
    EXPECT_TRUE(clock_cache_detail::MatchesPinned(*slot, Hash32(key, 0), key));
    EXPECT_EQ(TextOf(*pinned), "old");
    EXPECT_EQ(TextOf(*replacement), "new");
    EXPECT_GT(cache.GetStats().erased_bytes, 0U);
    pinned.reset();
    EXPECT_EQ(cache.GetStats().erased_bytes, 0U);
}

TEST(ClockBlockCacheProtocolTest, AcquiredOldRootKeepsItsMaskAcrossGrowthAndSlotReuse) {
    ClockBlockCache cache(16 * 4096 * 16);
    constexpr std::uint32_t Target = 0x0123456fU;
    for (std::uint32_t index = 0; index < 4; ++index) {
        ASSERT_TRUE(cache.Insert(CollidingKey(index, Target), ValueOf(std::to_string(index)), 4096)
                        .has_value());
    }
    const auto key = CollidingKey(0, Target);
    const auto old = ClockBlockCacheTestPeer::AcquireRoot(cache, key);
    ASSERT_NE(old.index, nullptr);
    ASSERT_EQ(old.index->bucket_count, 8U);
    ASSERT_NE(Target & old.index->mask, Target & std::size_t{15});
    ASSERT_TRUE(cache.Insert(CollidingKey(4, Target), ValueOf("4"), 4096).has_value());
    const auto grown = ClockBlockCacheTestPeer::AcquireRoot(cache, key);
    ASSERT_NE(grown.index, old.index);
    EXPECT_EQ(grown.index->bucket_count, 16U);
    {
        const auto found = ClockBlockCacheTestPeer::LookupInRoot(old, key);
        ASSERT_TRUE(found.has_value());
        EXPECT_EQ(TextOf(*found), "0");
    }
    clock_cache_detail::Slot* previous = nullptr;
    {
        const auto found = cache.Lookup(key);
        ASSERT_TRUE(found.has_value());
        previous = ClockBlockCacheTestPeer::Position(*found);
    }
    cache.Erase(key);
    const auto replacement_key = CollidingKey(5, Target);
    {
        auto replacement = cache.Insert(replacement_key, ValueOf("5"), 4096);
        ASSERT_TRUE(replacement.has_value());
        ASSERT_EQ(ClockBlockCacheTestPeer::Position(*replacement), previous);
    }
    EXPECT_FALSE(ClockBlockCacheTestPeer::LookupInRoot(old, key).has_value());
    for (std::uint32_t index = 1; index <= 5; ++index) {
        const auto found = cache.Lookup(CollidingKey(index, Target));
        ASSERT_TRUE(found.has_value()) << index;
        EXPECT_EQ(TextOf(*found), std::to_string(index));
    }
    EXPECT_GT(cache.GetStats().retained_index_bytes, 0U);
    EXPECT_EQ(cache.total_charge(), 5U * 4096U);
}

TEST(ClockBlockCacheProtocolTest, AcquiredRootRemainsUsableAfterInPlaceRefresh) {
    ClockBlockCache cache(16 * 4096 * 8);
    constexpr std::uint32_t Target = 0x0123456fU;
    for (std::uint32_t index = 0; index < 4; ++index) {
        ASSERT_TRUE(cache.Insert(CollidingKey(index, Target), ValueOf(std::to_string(index)), 4096)
                        .has_value());
    }
    const auto survivor = CollidingKey(3, Target);
    const auto old = ClockBlockCacheTestPeer::AcquireRoot(cache, survivor);
    const auto before = cache.GetStats();
    for (std::uint32_t index = 0; index < 3; ++index) {
        cache.Erase(CollidingKey(index, Target));
    }
    ASSERT_TRUE(cache.Insert(CollidingKey(8, Target ^ 1U), ValueOf("new"), 4096).has_value());
    const auto refreshed = ClockBlockCacheTestPeer::AcquireRoot(cache, survivor);
    EXPECT_EQ(refreshed.index, old.index);
    EXPECT_EQ(cache.GetStats().index_refreshes, before.index_refreshes + 1);
    const auto found = ClockBlockCacheTestPeer::LookupInRoot(old, survivor);
    ASSERT_TRUE(found.has_value());
    EXPECT_EQ(TextOf(*found), "3");
    EXPECT_FALSE(ClockBlockCacheTestPeer::LookupInRoot(old, CollidingKey(0, Target)).has_value());
    EXPECT_EQ(cache.GetStats().retained_index_bytes, before.retained_index_bytes);
    EXPECT_EQ(cache.total_charge(), 2U * 4096U);
}

TEST(ClockBlockCacheProtocolTest, LateErasedReleaseNeverReclaimsALiveOrPinnedReplacement) {
    enum class Replacement { Live, ErasedPinned, ErasedZero };
    for (const auto next :
         {Replacement::Live, Replacement::ErasedPinned, Replacement::ErasedZero}) {
        SCOPED_TRACE(static_cast<int>(next));
        ClockBlockCache cache(16 * 8);
        const auto old_key = CollidingKey(0);
        auto old = cache.Insert(old_key, ValueOf("old"), 1);
        ASSERT_TRUE(old.has_value());
        auto* slot = ClockBlockCacheTestPeer::Position(*old);
        for (std::uint32_t index = 1; index < 8; ++index) {
            ASSERT_TRUE(cache.Insert(CollidingKey(index), ValueOf("filler"), 1).has_value());
        }
        ASSERT_EQ(cache.GetStats().pooled_slots, 8U);
        ASSERT_EQ(cache.GetStats().indexed_entries, 8U);
        cache.Erase(old_key);
        auto pending = ClockBlockCacheTestPeer::ReleaseToPending(*old);
        ASSERT_TRUE(pending.has_value());
        EXPECT_EQ(ClockBlockCacheTestPeer::Position(*old), nullptr);
        const auto new_key = CollidingKey(8);
        auto replacement = cache.Insert(new_key, ValueOf("replacement"), 2);
        ASSERT_TRUE(replacement.has_value());
        ASSERT_EQ(ClockBlockCacheTestPeer::Position(*replacement), slot);
        if (next != Replacement::Live) {
            cache.Erase(new_key);
        }
        auto later = next == Replacement::ErasedZero
                         ? ClockBlockCacheTestPeer::ReleaseToPending(*replacement)
                         : std::optional<ClockBlockCacheTestPeer::PendingRelease>{};
        pending->Finish();
        if (next == Replacement::ErasedZero) {
            ASSERT_TRUE(later.has_value());
            EXPECT_EQ(cache.GetStats().erased_bytes, 0U);
            later->Finish();
            EXPECT_EQ(cache.GetStats().erased_bytes, 0U);
        } else {
            EXPECT_EQ(TextOf(*replacement), "replacement");
            if (next == Replacement::Live) {
                EXPECT_TRUE(cache.Lookup(new_key).has_value());
                EXPECT_EQ(cache.GetStats().erased_bytes, 0U);
            } else {
                EXPECT_FALSE(cache.Lookup(new_key).has_value());
                EXPECT_GT(cache.GetStats().erased_bytes, 0U);
            }
        }
        EXPECT_EQ(cache.total_charge(), next == Replacement::Live ? 8U : 6U);
        EXPECT_EQ(cache.GetStats().retired_bytes, 0U);
    }
}

TEST(ClockBlockCacheProtocolTest, PendingCleanupConsumesTheHandleAndFinishesOnUnwinding) {
    ClockBlockCache cache(16 * 4096);
    const auto key = CollidingKey(1);
    auto inserted = cache.Insert(key, ValueOf("value"), 4096);
    ASSERT_TRUE(inserted.has_value());
    cache.Erase(key);
    EXPECT_THROW(
        {
            auto pending = ClockBlockCacheTestPeer::ReleaseToPending(*inserted);
            if (!pending.has_value()) {
                throw std::logic_error("expected pending erased cleanup");
            }
            EXPECT_EQ(ClockBlockCacheTestPeer::Position(*inserted), nullptr);
            EXPECT_GT(cache.GetStats().erased_bytes, 0U);
            throw std::runtime_error("leave the pending-cleanup scope");
        },
        std::runtime_error);
    EXPECT_EQ(cache.GetStats().erased_bytes, 0U);
    EXPECT_EQ(cache.GetStats().retired_bytes, 0U);
}

TEST(ClockBlockCacheProtocolTest, StandaloneReleaseReturnsNoPendingWorkOrUsableSlot) {
    ClockBlockCache cache(0);
    auto inserted = cache.Insert(CollidingKey(1), ValueOf("standalone"), 4096);
    ASSERT_TRUE(inserted.has_value());
    EXPECT_GT(cache.GetStats().standalone_bytes, 0U);
    const auto pending = ClockBlockCacheTestPeer::ReleaseToPending(*inserted);
    EXPECT_FALSE(pending.has_value());
    EXPECT_EQ(ClockBlockCacheTestPeer::Position(*inserted), nullptr);
    EXPECT_EQ(cache.GetStats().standalone_bytes, 0U);
}

}  // namespace
}  // namespace modern_leveldb
