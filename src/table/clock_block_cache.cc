#include "table/clock_block_cache.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <expected>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

#include "cache/sharded_lru_cache_core.h"
#include "table/block.h"

namespace modern_leveldb::clock_cache_detail {
namespace {

constexpr std::uint64_t StateMask = 3;
constexpr std::uint64_t Empty = 0;
constexpr std::uint64_t Writing = 1;
constexpr std::uint64_t Live = 2;
constexpr std::uint64_t Erased = 3;
constexpr std::uint64_t Visited = 4;
constexpr std::uint64_t OnePin = 8;
constexpr std::size_t SegmentSlots = 8;
constexpr std::size_t PinAttempts = 8;
constexpr std::size_t Shards = ShardedLruCacheCore::ShardCount;
using Key = ClockBlockCache::Key;

void RecordPeak(std::atomic<std::size_t>& peak, std::size_t value) noexcept {
    auto previous = peak.load(std::memory_order_relaxed);
    while (previous < value &&
           !peak.compare_exchange_weak(previous, value, std::memory_order_relaxed)) {
    }
}

}  // namespace

struct Diagnostics {
    std::atomic<std::size_t> refs{1};
    std::atomic<std::size_t> standalone{0};
    std::atomic<std::size_t> peak_standalone{0};
    std::atomic<std::size_t> erased{0};
    std::atomic<std::size_t> peak_erased{0};
    std::atomic<std::size_t> peak_retired{0};
    std::atomic<std::size_t> retired{0};
    std::atomic<std::size_t> peak_preparation{0};

    void Retain() noexcept { refs.fetch_add(1, std::memory_order_relaxed); }
    void Release() noexcept {
        if (refs.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            delete this;
        }
    }
};

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

struct Entry {
    explicit Entry(std::unique_ptr<const Block> block_value) noexcept
        : block(std::move(block_value)) {
        standalone.owner = this;
        standalone.value = block.get();
    }

    std::unique_ptr<const Block> block;
    Entry* next_retired = nullptr;
    // Co-allocation prepares the uncached lease before any mapping invalidation.
    // Its complete storage is counted for every indexed owner too.
    Slot standalone;
    Diagnostics* accounting = nullptr;
    std::size_t external_bytes = 0;
    bool counted_standalone = false;
};

std::size_t OwnerBytes(const Entry& entry) noexcept {
    return sizeof(Entry) + sizeof(Block) + entry.block->size();
}

void Externalize(Entry& entry, Diagnostics& diagnostics, bool standalone) noexcept {
    entry.accounting = &diagnostics;
    entry.external_bytes = OwnerBytes(entry);
    entry.counted_standalone = standalone;
    diagnostics.Retain();
    auto& current = standalone ? diagnostics.standalone : diagnostics.erased;
    auto& peak = standalone ? diagnostics.peak_standalone : diagnostics.peak_erased;
    RecordPeak(peak, current.fetch_add(entry.external_bytes, std::memory_order_relaxed) +
                         entry.external_bytes);
}

void DeleteEntry(Entry* entry) noexcept {
    auto* diagnostics = entry->accounting;
    const auto bytes = entry->external_bytes;
    const auto standalone = entry->counted_standalone;
    delete entry;
    if (diagnostics != nullptr) {
        auto& current = standalone ? diagnostics->standalone : diagnostics->erased;
        current.fetch_sub(bytes, std::memory_order_relaxed);
        diagnostics->Release();
    }
}

struct Retired {
    explicit Retired(Diagnostics& counters) noexcept : diagnostics(counters) {}
    ~Retired() {
        while (head != nullptr) {
            auto* entry = head;
            head = entry->next_retired;
            const auto bytes = OwnerBytes(*entry);
            DeleteEntry(entry);
            diagnostics.retired.fetch_sub(bytes, std::memory_order_relaxed);
        }
    }

    void Push(Entry& entry) noexcept {
        const auto bytes = OwnerBytes(entry);
        RecordPeak(diagnostics.peak_retired,
                   diagnostics.retired.fetch_add(bytes, std::memory_order_relaxed) + bytes);
        entry.next_retired = head;
        head = &entry;
    }

    Diagnostics& diagnostics;
    Entry* head = nullptr;
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

struct Growth {
    std::unique_ptr<Index> index;
    std::unique_ptr<Slot[]> segment;
    // Count constructors permit debug-library bookkeeping failures to propagate.
    std::vector<std::unique_ptr<Index>> root_owners = std::vector<std::unique_ptr<Index>>(0);
    std::vector<std::unique_ptr<Slot[]>> segment_owners = std::vector<std::unique_ptr<Slot[]>>(0);
};

struct Shard {
    mutable std::mutex mutex;
    std::size_t capacity = 0;
    std::size_t allowance = 0;
    std::size_t fixed_bytes = 0;
    Diagnostics* diagnostics = nullptr;
    std::atomic<Index*> index{nullptr};
    Slot tombstone;
    std::vector<std::unique_ptr<Index>> roots = std::vector<std::unique_ptr<Index>>(0);
    std::vector<std::unique_ptr<Slot[]>> segments = std::vector<std::unique_ptr<Slot[]>>(0);
    Slot* free = nullptr;
    std::size_t clock_hand = 0;
    std::size_t usage = 0;
    std::size_t live_entries = 0;
    std::size_t tombstones = 0;
    std::size_t rejected = 0;
    std::size_t refreshes = 0;

    [[nodiscard]] std::size_t slot_count() const noexcept { return segments.size() * SegmentSlots; }
    [[nodiscard]] Slot& Position(std::size_t position) noexcept {
        return segments[position / SegmentSlots][position % SegmentSlots];
    }
    [[nodiscard]] std::size_t StorageBytes(const Growth* growth = nullptr) const noexcept;
    [[nodiscard]] bool Fits(const Growth& growth, std::size_t resident) const noexcept;
    [[nodiscard]] bool Prepare(Growth& growth, std::size_t resident, bool need_segment);
    void Publish(Growth& growth) noexcept;
    void Rehash(Index& target) noexcept;
    void Refresh() noexcept;
    [[nodiscard]] Slot* Find(Key key, std::uint32_t hash) const noexcept;
    void IndexSlot(Slot& slot) noexcept;
    void RemoveIndex(Slot& slot) noexcept;
    void Erase(Slot& slot, Retired& retired) noexcept;
    void Reclaim(Slot& slot, Retired& retired) noexcept;
    void FreeWriting(Slot& slot) noexcept;
    [[nodiscard]] Slot* Reserve(Retired& retired, std::size_t& scan_budget) noexcept;
    [[nodiscard]] Slot* ReserveVictim(Retired& retired, std::size_t& scan_budget) noexcept;
    void RetireWriting(Slot& slot, Retired& retired) noexcept;
};

std::size_t Shard::StorageBytes(const Growth* growth) const noexcept {
    auto bytes = fixed_bytes + slot_count() * sizeof(Slot);
    for (const auto& root : roots) {
        bytes += root->bytes();
    }
    auto root_capacity = roots.capacity();
    auto segment_capacity = segments.capacity();
    if (growth != nullptr) {
        if (growth->index != nullptr) {
            bytes += growth->index->bytes();
        }
        if (growth->segment != nullptr) {
            bytes += SegmentSlots * sizeof(Slot);
        }
        if (growth->root_owners.capacity() != 0) {
            root_capacity = growth->root_owners.capacity();
        }
        if (growth->segment_owners.capacity() != 0) {
            segment_capacity = growth->segment_owners.capacity();
        }
    }
    return bytes + root_capacity * sizeof(std::unique_ptr<Index>) +
           segment_capacity * sizeof(std::unique_ptr<Slot[]>);
}

bool Shard::Fits(const Growth& growth, std::size_t resident) const noexcept {
    const auto storage = StorageBytes(&growth);
    return storage <= allowance &&
           resident <= (allowance - storage) / (sizeof(Entry) + sizeof(Block));
}

bool Shard::Prepare(Growth& growth, std::size_t resident, bool need_segment) {
    const auto* current = index.load(std::memory_order_acquire);
    const auto buckets = current == nullptr                     ? std::size_t{8}
                         : resident > current->bucket_count / 2 ? current->bucket_count * 2
                                                                : current->bucket_count;
    if (buckets > (allowance - std::min(allowance, sizeof(Index))) / sizeof(std::atomic<Slot*>)) {
        return false;
    }
    if (current == nullptr || buckets != current->bucket_count) {
        growth.index = std::make_unique<Index>(buckets);
        if (roots.size() == roots.capacity()) {
            growth.root_owners.reserve(std::max<std::size_t>(1, roots.size() * 2));
        }
    }
    if (need_segment) {
        if (SegmentSlots * sizeof(Slot) > allowance) {
            return false;
        }
        growth.segment = std::make_unique<Slot[]>(SegmentSlots);
        for (std::size_t position = 0; position < SegmentSlots; ++position) {
            growth.segment[position].shard = this;
            growth.segment[position].clock_position = slot_count() + position;
        }
        if (segments.size() == segments.capacity()) {
            growth.segment_owners.reserve(std::max<std::size_t>(1, segments.size() * 2));
        }
    }
    RecordPeak(diagnostics->peak_preparation,
               StorageBytes() + (growth.index == nullptr ? 0 : growth.index->bytes()) +
                   (growth.segment == nullptr ? 0 : SegmentSlots * sizeof(Slot)) +
                   growth.root_owners.capacity() * sizeof(std::unique_ptr<Index>) +
                   growth.segment_owners.capacity() * sizeof(std::unique_ptr<Slot[]>) +
                   resident * (sizeof(Entry) + sizeof(Block)));
    return Fits(growth, resident);
}

void Shard::Publish(Growth& growth) noexcept {
    if (growth.root_owners.capacity() != 0) {
        for (auto& root : roots) {
            growth.root_owners.push_back(std::move(root));
        }
        roots.swap(growth.root_owners);
    }
    if (growth.segment_owners.capacity() != 0) {
        for (auto& segment : segments) {
            growth.segment_owners.push_back(std::move(segment));
        }
        segments.swap(growth.segment_owners);
    }
    if (growth.segment != nullptr) {
        auto* first = growth.segment.get();
        segments.push_back(std::move(growth.segment));
        for (std::size_t position = 0; position < SegmentSlots; ++position) {
            first[position].next_free = free;
            free = &first[position];
        }
    }
    if (growth.index != nullptr) {
        Rehash(*growth.index);
        auto* published = growth.index.get();
        roots.push_back(std::move(growth.index));
        tombstones = 0;
        index.store(published, std::memory_order_release);
    }
}

void Shard::Rehash(Index& target) noexcept {
    for (std::size_t position = 0; position < target.bucket_count; ++position) {
        target.buckets[position].store(nullptr, std::memory_order_release);
    }
    for (std::size_t position = 0; position < slot_count(); ++position) {
        auto& slot = Position(position);
        if ((slot.metadata.load(std::memory_order_acquire) & StateMask) != Live) {
            continue;
        }
        auto bucket = slot.hash & target.mask;
        while (target.buckets[bucket].load(std::memory_order_acquire) != nullptr) {
            bucket = (bucket + 1) & target.mask;
        }
        slot.bucket_position = bucket;
        target.buckets[bucket].store(&slot, std::memory_order_release);
    }
}

void Shard::Refresh() noexcept {
    Rehash(*index.load(std::memory_order_acquire));
    tombstones = 0;
    ++refreshes;
}

Slot* Shard::Find(Key key, std::uint32_t hash) const noexcept {
    const auto* current = index.load(std::memory_order_acquire);
    if (current == nullptr) {
        return nullptr;
    }
    auto bucket = hash & current->mask;
    for (std::size_t probe = 0; probe < current->bucket_count; ++probe) {
        auto* slot = current->buckets[bucket].load(std::memory_order_acquire);
        if (slot == nullptr) {
            return nullptr;
        }
        if (slot != &tombstone && slot->hash == hash &&
            (slot->metadata.load(std::memory_order_acquire) & StateMask) == Live &&
            std::memcmp(slot->key.data(), key.data(), key.size()) == 0) {
            return slot;
        }
        bucket = (bucket + 1) & current->mask;
    }
    return nullptr;
}

void Shard::IndexSlot(Slot& slot) noexcept {
    auto* current = index.load(std::memory_order_acquire);
    auto bucket = slot.hash & current->mask;
    // Preparation keeps at least one free bucket in this complete probe domain.
    for (;;) {
        auto* previous = current->buckets[bucket].load(std::memory_order_acquire);
        if (previous == nullptr || previous == &tombstone) {
            if (previous == &tombstone) {
                --tombstones;
            }
            slot.bucket_position = bucket;
            current->buckets[bucket].store(&slot, std::memory_order_release);
            return;
        }
        bucket = (bucket + 1) & current->mask;
    }
}

void Shard::RemoveIndex(Slot& slot) noexcept {
    index.load(std::memory_order_acquire)
        ->buckets[slot.bucket_position]
        .store(&tombstone, std::memory_order_release);
    ++tombstones;
    usage -= slot.charge;
    --live_entries;
}

void Shard::FreeWriting(Slot& slot) noexcept {
    slot.value = nullptr;
    slot.owner = nullptr;
    slot.next_free = free;
    free = &slot;
    slot.metadata.store(Empty, std::memory_order_release);
}

void Shard::Reclaim(Slot& slot, Retired& retired) noexcept {
    retired.Push(*slot.owner);
    FreeWriting(slot);
}

void Shard::Erase(Slot& slot, Retired& retired) noexcept {
    auto previous = slot.metadata.load(std::memory_order_acquire);
    while (!slot.metadata.compare_exchange_weak(previous, (previous & ~StateMask) | Erased,
                                                std::memory_order_acq_rel,
                                                std::memory_order_acquire)) {
    }
    RemoveIndex(slot);
    if (previous / OnePin == 0) {
        slot.metadata.exchange(Writing, std::memory_order_acq_rel);
        Reclaim(slot, retired);
    } else {
        Externalize(*slot.owner, *diagnostics, false);
    }
}

void Shard::RetireWriting(Slot& slot, Retired& retired) noexcept {
    RemoveIndex(slot);
    retired.Push(*slot.owner);
    slot.owner = nullptr;
    slot.value = nullptr;
}

Slot* Shard::Reserve(Retired& retired, std::size_t& scan_budget) noexcept {
    if (free != nullptr) {
        auto* slot = free;
        free = slot->next_free;
        slot->metadata.exchange(Writing, std::memory_order_acq_rel);
        return slot;
    }
    return ReserveVictim(retired, scan_budget);
}

Slot* Shard::ReserveVictim(Retired& retired, std::size_t& scan_budget) noexcept {
    const auto count = slot_count();
    while (scan_budget != 0) {
        --scan_budget;
        auto& slot = Position(clock_hand);
        clock_hand = clock_hand + 1 == count ? 0 : clock_hand + 1;
        auto previous = slot.metadata.load(std::memory_order_acquire);
        if ((previous & StateMask) == Erased && previous / OnePin == 0) {
            slot.metadata.exchange(Writing, std::memory_order_acq_rel);
            retired.Push(*slot.owner);
            slot.owner = nullptr;
            slot.value = nullptr;
            return &slot;
        }
        if (previous / OnePin != 0 || (previous & StateMask) != Live) {
            continue;
        }
        if ((previous & Visited) != 0) {
            static_cast<void>(slot.metadata.compare_exchange_strong(previous, previous & ~Visited,
                                                                    std::memory_order_acq_rel,
                                                                    std::memory_order_acquire));
            continue;
        }
        if (slot.metadata.compare_exchange_strong(previous, Writing, std::memory_order_acq_rel,
                                                  std::memory_order_acquire)) {
            RetireWriting(slot, retired);
            return &slot;
        }
    }
    return nullptr;
}

bool TryPin(Slot& slot) noexcept {
    auto previous = slot.metadata.load(std::memory_order_acquire);
    for (std::size_t attempt = 0; attempt < PinAttempts; ++attempt) {
        if ((previous & StateMask) != Live) {
            return false;
        }
        // The 61-bit count exceeds the number of addressable lease objects.
        const auto pinned = (previous + OnePin) | Visited;
        if (slot.metadata.compare_exchange_strong(previous, pinned, std::memory_order_acquire,
                                                  std::memory_order_relaxed)) {
            return true;
        }
    }
    return false;
}

void Release(Slot& slot) noexcept {
    if (slot.shard == nullptr) {
        DeleteEntry(slot.owner);
        return;
    }
    auto* shard = slot.shard;
    const auto previous = slot.metadata.fetch_sub(OnePin, std::memory_order_release);
    if ((previous & StateMask) != Erased || previous / OnePin != 1) {
        return;
    }
    Retired retired(*shard->diagnostics);
    {
        const std::lock_guard lock(shard->mutex);
        auto observed = slot.metadata.load(std::memory_order_acquire);
        if ((observed & StateMask) == Erased && observed / OnePin == 0) {
            slot.metadata.exchange(Writing, std::memory_order_acq_rel);
            shard->Reclaim(slot, retired);
        }
    }
}

struct State {
    explicit State(std::size_t capacity)
        : diagnostics(new Diagnostics), allowance(std::max<std::size_t>(64 * 1024, capacity / 4)) {
        const auto fixed = (sizeof(State) - sizeof(shards) + sizeof(Diagnostics) +
                            sizeof(ClockBlockCache) + Shards - 1) /
                           Shards;
        for (std::size_t position = 0; position < Shards; ++position) {
            auto& shard = shards[position];
            shard.capacity = ShardedLruCacheCore::ShardCapacity(capacity);
            shard.allowance = allowance / Shards + (position < allowance % Shards ? 1U : 0U);
            shard.fixed_bytes = sizeof(Shard) + fixed;
            shard.diagnostics = diagnostics;
        }
    }

    ~State() {
        for (auto& shard : shards) {
            for (std::size_t position = 0; position < shard.slot_count(); ++position) {
                auto& slot = shard.Position(position);
                if (slot.owner != nullptr) {
                    DeleteEntry(slot.owner);
                    slot.owner = nullptr;
                }
            }
        }
        diagnostics->Release();
    }

    Diagnostics* diagnostics;
    std::size_t allowance;
    std::atomic<std::uint64_t> next_id{0};
    std::array<Shard, Shards> shards;
};

}  // namespace modern_leveldb::clock_cache_detail

namespace modern_leveldb {
namespace clock_detail = clock_cache_detail;

ClockBlockCache::Handle::Handle(clock_detail::Slot& slot) noexcept : slot_(&slot) {}

ClockBlockCache::Handle::Handle(Handle&& source) noexcept
    : slot_(std::exchange(source.slot_, nullptr)) {}

ClockBlockCache::Handle& ClockBlockCache::Handle::operator=(Handle&& source) noexcept {
    if (this != &source) {
        Reset();
        slot_ = std::exchange(source.slot_, nullptr);
    }
    return *this;
}

ClockBlockCache::Handle::~Handle() { Reset(); }

void ClockBlockCache::Handle::Reset() noexcept {
    if (auto* slot = std::exchange(slot_, nullptr)) {
        clock_detail::Release(*slot);
    }
}

const Block& ClockBlockCache::Handle::value() const noexcept { return *slot_->value; }

ClockBlockCache::ClockBlockCache(std::size_t capacity)
    : state_(std::make_unique<clock_detail::State>(capacity)) {}

ClockBlockCache::~ClockBlockCache() = default;

Result<ClockBlockCache::Handle> ClockBlockCache::Insert(Key key, std::unique_ptr<const Block> value,
                                                        std::size_t charge) {
    if (value == nullptr) {
        return std::unexpected(Error::InvalidArgument("cache value is null"));
    }
    if (charge == 0) {
        return std::unexpected(Error::InvalidArgument("cache charge must be positive"));
    }
    auto entry = std::make_unique<clock_detail::Entry>(std::move(value));
    clock_detail::Retired retired(*state_->diagnostics);
    const auto hash = ShardedLruCacheCore::HashKey(key);
    auto& shard = state_->shards[ShardedLruCacheCore::ShardIndex(hash)];
    clock_detail::Slot* retained = nullptr;
    std::optional<Handle> lease;
    {
        const std::lock_guard lock(shard.mutex);
        auto* previous = shard.Find(key, hash);
        bool can_retain = shard.capacity != 0 && charge <= shard.capacity;
        clock_detail::Growth growth;
        if (can_retain) {
            const auto without_previous =
                shard.usage - (previous == nullptr ? 0 : previous->charge);
            const bool previous_will_free =
                previous != nullptr &&
                previous->metadata.load(std::memory_order_acquire) / clock_detail::OnePin == 0;
            const bool need_segment = shard.free == nullptr && !previous_will_free &&
                                      without_previous <= shard.capacity - charge;
            can_retain = shard.Prepare(growth, shard.live_entries + (previous == nullptr ? 1U : 0U),
                                       need_segment);
            if (can_retain) {
                shard.Publish(growth);
            }
        }
        if (previous != nullptr) {
            shard.Erase(*previous, retired);
        }
        if (can_retain) {
            auto scan_budget = shard.slot_count() * 3;
            retained = shard.Reserve(retired, scan_budget);
            if (retained != nullptr) {
                while (shard.usage > shard.capacity - charge) {
                    auto* victim = shard.ReserveVictim(retired, scan_budget);
                    if (victim == nullptr) {
                        shard.FreeWriting(*retained);
                        retained = nullptr;
                        break;
                    }
                    shard.FreeWriting(*victim);
                }
            }
        }
        if (retained != nullptr) {
            if (shard.tombstones > shard.index.load(std::memory_order_acquire)->bucket_count / 4) {
                shard.Refresh();
            }
            std::ranges::copy(key, retained->key.begin());
            retained->hash = hash;
            retained->charge = charge;
            retained->value = entry->block.get();
            retained->owner = entry.release();
            retained->metadata.store(
                clock_detail::Live | clock_detail::Visited | clock_detail::OnePin,
                std::memory_order_release);
            lease.emplace(Handle(*retained));
            shard.IndexSlot(*retained);
            shard.usage += charge;
            ++shard.live_entries;
        } else {
            ++shard.rejected;
            clock_detail::Externalize(*entry, *state_->diagnostics, true);
            retained = &entry->standalone;
            retained->metadata.store(clock_detail::Erased | clock_detail::OnePin,
                                     std::memory_order_relaxed);
            lease.emplace(Handle(*retained));
            static_cast<void>(entry.release());
        }
    }
    // The counted result reference already protects publication and reentrant disposal.
    return std::move(*lease);
}

std::optional<ClockBlockCache::Handle> ClockBlockCache::Lookup(Key key) {
    const auto hash = ShardedLruCacheCore::HashKey(key);
    auto& shard = state_->shards[ShardedLruCacheCore::ShardIndex(hash)];
    const auto* index = shard.index.load(std::memory_order_acquire);
    if (index == nullptr) {
        return std::nullopt;
    }
    auto bucket = hash & index->mask;
    for (std::size_t probe = 0; probe < index->bucket_count; ++probe) {
        auto* slot = index->buckets[bucket].load(std::memory_order_acquire);
        if (slot == nullptr) {
            return std::nullopt;
        }
        if (slot != &shard.tombstone && clock_detail::TryPin(*slot)) {
            if (slot->hash == hash && std::memcmp(slot->key.data(), key.data(), key.size()) == 0) {
                return Handle(*slot);
            }
            clock_detail::Release(*slot);
        }
        bucket = (bucket + 1) & index->mask;
    }
    return std::nullopt;
}

void ClockBlockCache::Erase(Key key) {
    const auto hash = ShardedLruCacheCore::HashKey(key);
    auto& shard = state_->shards[ShardedLruCacheCore::ShardIndex(hash)];
    clock_detail::Retired retired(*state_->diagnostics);
    {
        const std::lock_guard lock(shard.mutex);
        if (auto* slot = shard.Find(key, hash)) {
            shard.Erase(*slot, retired);
        }
    }
}

std::size_t ClockBlockCache::total_charge() const {
    std::size_t charge = 0;
    for (const auto& shard : state_->shards) {
        const std::lock_guard lock(shard.mutex);
        if (shard.usage > std::numeric_limits<std::size_t>::max() - charge) {
            return std::numeric_limits<std::size_t>::max();
        }
        charge += shard.usage;
    }
    return charge;
}

ClockCacheStats ClockBlockCache::GetStats() const {
    ClockCacheStats result;
    for (const auto& shard : state_->shards) {
        const std::lock_guard lock(shard.mutex);
        result.metadata_bytes += shard.StorageBytes() +
                                 shard.live_entries * (sizeof(clock_detail::Entry) + sizeof(Block));
        result.metadata_allowance += shard.allowance;
        result.pooled_slots += shard.slot_count();
        result.indexed_entries += shard.live_entries;
        const auto* current = shard.index.load(std::memory_order_acquire);
        for (const auto& index : shard.roots) {
            if (index.get() != current) {
                result.retained_index_bytes += index->bytes();
            }
        }
        result.admission_rejections += shard.rejected;
        result.index_refreshes += shard.refreshes;
    }
    const auto& counters = *state_->diagnostics;
    result.standalone_bytes = counters.standalone.load(std::memory_order_relaxed);
    result.peak_standalone_bytes = counters.peak_standalone.load(std::memory_order_relaxed);
    result.erased_bytes = counters.erased.load(std::memory_order_relaxed);
    result.peak_erased_bytes = counters.peak_erased.load(std::memory_order_relaxed);
    result.peak_retired_bytes = counters.peak_retired.load(std::memory_order_relaxed);
    result.retired_bytes = counters.retired.load(std::memory_order_relaxed);
    result.peak_shard_preparation_bytes = counters.peak_preparation.load(std::memory_order_relaxed);
    return result;
}

std::uint64_t ClockBlockCache::NewId() noexcept {
    return state_->next_id.fetch_add(1, std::memory_order_relaxed) + 1;
}

}  // namespace modern_leveldb
