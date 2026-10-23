#ifndef MODERN_LEVELDB_CACHE_SHARDED_LRU_CACHE_CORE_H_
#define MODERN_LEVELDB_CACHE_SHARDED_LRU_CACHE_CORE_H_

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>

#include "modern_leveldb/base/bytes.h"
#include "modern_leveldb/base/hash.h"

namespace modern_leveldb {

// Sharded retention and eviction, separate from value type. Capacity is a per-shard
// target (ceil(total / 16)); pinned entries may exceed it. A pin borrows this core,
// so all pins must be released before destruction.
class ShardedLruCacheCore final {
private:
    struct Entry;

public:
    using ValueDeleter = void (*)(const void*) noexcept;

    static constexpr std::size_t ShardCount = 16;
    static constexpr unsigned int ShardBits = 4;

    [[nodiscard]] static std::uint32_t HashKey(ByteView key) noexcept { return Hash32(key, 0U); }
    [[nodiscard]] static constexpr std::size_t ShardIndex(std::uint32_t hash) noexcept {
        return hash >> (32U - ShardBits);
    }
    [[nodiscard]] static constexpr std::size_t ShardCapacity(std::size_t capacity) noexcept {
        return capacity / ShardCount + (capacity % ShardCount != 0U ? 1U : 0U);
    }

    class Pin final {
    public:
        Pin(const Pin&) = delete;
        Pin& operator=(const Pin&) = delete;
        Pin(Pin&& source) noexcept;
        Pin& operator=(Pin&& source) noexcept;
        ~Pin();

        [[nodiscard]] const void* value() const noexcept;

    private:
        friend class ShardedLruCacheCore;

        Pin(ShardedLruCacheCore& cache, Entry& entry) noexcept;
        void Reset() noexcept;

        ShardedLruCacheCore* cache_ = nullptr;
        Entry* entry_ = nullptr;
    };

    explicit ShardedLruCacheCore(std::size_t capacity);
    ShardedLruCacheCore(const ShardedLruCacheCore&) = delete;
    ShardedLruCacheCore& operator=(const ShardedLruCacheCore&) = delete;
    ShardedLruCacheCore(ShardedLruCacheCore&&) = delete;
    ShardedLruCacheCore& operator=(ShardedLruCacheCore&&) = delete;
    ~ShardedLruCacheCore();

    [[nodiscard]] Pin Insert(ByteView key, const void* value, ValueDeleter deleter,
                             std::size_t charge);
    [[nodiscard]] std::optional<Pin> Lookup(ByteView key);
    void Erase(ByteView key);
    // Charge of entries still indexed in the cache, not all retained values or
    // physical memory. Uncached/erased pins are outside this accounting.
    [[nodiscard]] std::size_t total_charge() const;
    [[nodiscard]] std::uint64_t NewId() noexcept;

private:
    struct Entry {
        Entry(const void* entry_value, ValueDeleter entry_deleter, std::size_t entry_charge,
              std::size_t entry_key_length, std::uint32_t entry_hash) noexcept
            : value(entry_value),
              deleter(entry_deleter),
              charge(entry_charge),
              key_length(entry_key_length),
              hash(entry_hash) {}

        [[nodiscard]] ByteView key() const noexcept;
        [[nodiscard]] bool Matches(ByteView key, std::uint32_t hash) const noexcept;

        const void* value;
        ValueDeleter deleter;
        Entry* next_hash = nullptr;
        Entry* next = nullptr;
        Entry* previous = nullptr;
        std::size_t charge;
        std::size_t key_length;
        // Cache ownership contributes one reference; each pin adds another.
        bool in_cache = false;
        std::uint32_t refs = 1;
        std::uint32_t hash;
    };

    class HandleTable final {
    public:
        struct PreparedInsert {
            Entry** position;
            Entry* existing;
            bool needs_growth;
        };

        HandleTable();
        HandleTable(const HandleTable&) = delete;
        HandleTable& operator=(const HandleTable&) = delete;
        ~HandleTable() = default;

        [[nodiscard]] Entry* Lookup(ByteView key, std::uint32_t hash) const noexcept;
        [[nodiscard]] PreparedInsert PrepareInsert(ByteView key, std::uint32_t hash) noexcept;
        Entry* CommitInsert(PreparedInsert prepared, Entry* entry);
        Entry* Remove(ByteView key, std::uint32_t hash) noexcept;

    private:
        [[nodiscard]] Entry** FindPointer(ByteView key, std::uint32_t hash) noexcept;
        [[nodiscard]] static std::unique_ptr<Entry*[]> PrepareGrowth(std::uint32_t growth_length);
        Entry* LinkEntry(PreparedInsert prepared, Entry* entry) noexcept;
        [[nodiscard]] static std::unique_ptr<Entry*[]> AllocateBuckets(std::uint32_t length);
        static void RehashChain(Entry* entry, Entry** buckets, std::uint32_t length) noexcept;
        void RehashInto(std::unique_ptr<Entry*[]> buckets, std::uint32_t length) noexcept;

        std::uint32_t length_ = 0;
        std::uint32_t elements_ = 0;
        std::unique_ptr<Entry*[]> buckets_;
    };

    struct Shard {
        Shard();

        mutable std::mutex mutex;
        std::size_t capacity = 0;
        std::size_t usage = 0;
        HandleTable table;
        // Indexed entries with only the cache reference are evictable in lru.
        // Indexed entries with pins sit in in_use; erased pins sit in neither list.
        Entry lru;
        Entry in_use;
    };

    struct ErasedValueDeleter {
        ValueDeleter deleter;
        void operator()(const void* value) const noexcept;
    };

    using PendingValue = std::unique_ptr<const void, ErasedValueDeleter>;
    using PendingEntry = std::unique_ptr<Entry, void (*)(Entry*) noexcept>;

    [[nodiscard]] static bool TryAddCharge(std::size_t& total, std::size_t charge) noexcept;
    [[nodiscard]] static PendingEntry CreateEntry(ByteView key, PendingValue value,
                                                  std::size_t charge);
    static void CopyEntryKey(Entry& entry, ByteView key);
    [[nodiscard]] static bool CanAdmit(const Shard& shard, const Entry& entry,
                                       HandleTable::PreparedInsert prepared) noexcept;
    static void CommitAdmission(Shard& shard, Entry& entry, HandleTable::PreparedInsert prepared,
                                Entry*& retired);
    static void DeleteEntry(Entry* entry) noexcept;
    static void Remove(Entry& entry) noexcept;
    static void Append(Entry& list, Entry& entry) noexcept;
    static void MoveToList(Entry& list, Entry& entry) noexcept;
    static void Ref(Shard& shard, Entry& entry) noexcept;
    static void Unref(Shard& shard, Entry& entry, Entry*& retired) noexcept;
    [[nodiscard]] static Entry* LookupAndRef(Shard& shard, ByteView key,
                                             std::uint32_t hash) noexcept;
    [[nodiscard]] Entry* LookupAndRef(ByteView key);
    static void EraseMapping(Shard& shard, ByteView key, std::uint32_t hash,
                             Entry*& retired) noexcept;
    static void FinishErase(Shard& shard, Entry* entry, Entry*& retired) noexcept;
    static void EvictOldest(Shard& shard, Entry*& retired) noexcept;
    static void Evict(Shard& shard, Entry*& retired) noexcept;
    static void Retire(Entry& entry, Entry*& retired) noexcept;
    static void DeleteRetired(Entry* retired) noexcept;
    static void DestroyShard(Shard& shard, Entry*& retired) noexcept;
    void Initialize(std::size_t capacity) noexcept;
    void Destroy() noexcept;
    void Release(Entry& entry) noexcept;

    std::array<Shard, ShardCount> shards_;
    std::atomic<std::uint64_t> next_id_{0};
};

inline ShardedLruCacheCore::Pin::Pin(ShardedLruCacheCore& cache, Entry& entry) noexcept
    : cache_(&cache), entry_(&entry) {}

inline ShardedLruCacheCore::Pin::Pin(Pin&& source) noexcept
    : cache_(std::exchange(source.cache_, nullptr)),
      entry_(std::exchange(source.entry_, nullptr)) {}

inline ShardedLruCacheCore::Pin& ShardedLruCacheCore::Pin::operator=(Pin&& source) noexcept {
    if (this != &source) {
        Reset();
        cache_ = std::exchange(source.cache_, nullptr);
        entry_ = std::exchange(source.entry_, nullptr);
    }
    return *this;
}

inline ShardedLruCacheCore::Pin::~Pin() { Reset(); }

inline const void* ShardedLruCacheCore::Pin::value() const noexcept { return entry_->value; }

inline void ShardedLruCacheCore::Pin::Reset() noexcept {
    if (entry_ != nullptr) {
        cache_->Release(*entry_);
        cache_ = nullptr;
        entry_ = nullptr;
    }
}

inline std::optional<ShardedLruCacheCore::Pin> ShardedLruCacheCore::Lookup(ByteView key) {
    Entry* entry = LookupAndRef(key);
    if (entry == nullptr) {
        return std::nullopt;
    }
    return Pin(*this, *entry);
}

}  // namespace modern_leveldb

#endif  // MODERN_LEVELDB_CACHE_SHARDED_LRU_CACHE_CORE_H_
