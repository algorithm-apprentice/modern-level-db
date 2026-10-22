#ifndef MODERN_LEVELDB_TABLE_CLOCK_BLOCK_CACHE_PROTOCOL_H_
#define MODERN_LEVELDB_TABLE_CLOCK_BLOCK_CACHE_PROTOCOL_H_

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>

#include "table/clock_block_cache.h"

namespace modern_leveldb::clock_cache_detail {

inline constexpr std::uint64_t StateMask = 3;
inline constexpr std::uint64_t Empty = 0;
inline constexpr std::uint64_t Writing = 1;
inline constexpr std::uint64_t Live = 2;
inline constexpr std::uint64_t Erased = 3;
inline constexpr std::uint64_t Visited = 4;
inline constexpr std::uint64_t OnePin = 8;

struct Shard;
struct Entry;

struct Slot {
    std::atomic<std::uint64_t> metadata{Empty};
    std::array<std::byte, 16> key{};
    const Block* value = nullptr;
    Entry* owner = nullptr;
    // These identities never change after the position becomes reachable.
    Shard* shard = nullptr;
    std::size_t clock_position = 0;
    // Writer-only membership bookkeeping is not read by a lookup or lease.
    std::size_t bucket_position = 0;
    Slot* next_free = nullptr;
    std::size_t charge = 0;
    std::uint32_t hash = 0;
};

struct Index {
    explicit Index(std::size_t count)
        : bucket_count(count),
          mask(count - 1),
          buckets(std::make_unique<std::atomic<Slot*>[]>(count)) {}

    [[nodiscard]] std::size_t bytes() const noexcept {
        return sizeof(Index) + bucket_count * sizeof(std::atomic<Slot*>);
    }

    const std::size_t bucket_count;
    const std::size_t mask;
    std::unique_ptr<std::atomic<Slot*>[]> buckets;
};

// Borrows one acquired root and permanent sentinel, not a payload. The hash
// belongs to the key being probed; cache lifetime covers the complete attempt.
struct LookupRoot {
    const Index* index;
    const Slot* tombstone;
    std::uint32_t hash;
};

// TryPin and the deterministic peer establish a Live observed word first.
// Success owns a pin before field access; failure refreshes the observation.
[[nodiscard]] inline bool PinOnce(Slot& slot, std::uint64_t& observed) noexcept {
    // The 61-bit count exceeds the number of addressable lease objects.
    const auto pinned = (observed + OnePin) | Visited;
    return slot.metadata.compare_exchange_strong(observed, pinned, std::memory_order_acquire,
                                                 std::memory_order_relaxed);
}

// The caller owns a successfully acquired pin through both comparisons.
[[nodiscard]] inline bool MatchesPinned(const Slot& slot, std::uint32_t hash,
                                        ClockBlockCache::Key key) noexcept {
    return slot.hash == hash && std::memcmp(slot.key.data(), key.data(), key.size()) == 0;
}

// Only a pooled Erased last-pin transition returns pending cleanup. Null can
// mean a standalone slot was destroyed; never inspect that slot afterward.
[[nodiscard]] Shard* DropPin(Slot& slot) noexcept;
// Rechecks current eligibility under the permanent shard's writer mutex.
void FinishRelease(Slot& slot, Shard& shard) noexcept;

}  // namespace modern_leveldb::clock_cache_detail

#endif  // MODERN_LEVELDB_TABLE_CLOCK_BLOCK_CACHE_PROTOCOL_H_
