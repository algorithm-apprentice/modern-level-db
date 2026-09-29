#ifndef MODERN_LEVELDB_CACHE_SHARDED_LRU_CACHE_CORE_H_
#define MODERN_LEVELDB_CACHE_SHARDED_LRU_CACHE_CORE_H_

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>

#include "modern_leveldb/base/bytes.h"

namespace modern_leveldb {

class ShardedLruCacheCore final {
 private:
  struct Entry;

 public:
  using ValueDeleter = void (*)(const void*) noexcept;

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
  [[nodiscard]] std::size_t total_charge() const;
  [[nodiscard]] std::uint64_t NewId() noexcept;

 private:
  static constexpr std::size_t ShardCount = 16;
  static constexpr unsigned int ShardBits = 4;

  struct Entry {
    Entry(const void* entry_value, ValueDeleter entry_deleter, std::size_t entry_charge,
          std::size_t entry_key_length, std::uint32_t entry_hash) noexcept
        : value(entry_value),
          deleter(entry_deleter),
          charge(entry_charge),
          key_length(entry_key_length),
          hash(entry_hash) {}

    [[nodiscard]] ByteView key() const noexcept;

    const void* value;
    ValueDeleter deleter;
    Entry* next_hash = nullptr;
    Entry* next = nullptr;
    Entry* previous = nullptr;
    std::size_t charge;
    std::size_t key_length;
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
    [[nodiscard]] static std::unique_ptr<Entry*[]> AllocateBuckets(std::uint32_t length);
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
    Entry lru;
    Entry in_use;
  };

  struct ErasedValueDeleter {
    ValueDeleter deleter;
    void operator()(const void* value) const noexcept;
  };

  using PendingValue = std::unique_ptr<const void, ErasedValueDeleter>;
  using PendingEntry = std::unique_ptr<Entry, void (*)(Entry*) noexcept>;

  [[nodiscard]] static std::size_t ShardIndex(std::uint32_t hash) noexcept;
  [[nodiscard]] static PendingEntry CreateEntry(ByteView key, PendingValue value,
                                                std::size_t charge);
  static void DeleteEntry(Entry* entry) noexcept;
  static void Remove(Entry& entry) noexcept;
  static void Append(Entry& list, Entry& entry) noexcept;
  static void Ref(Shard& shard, Entry& entry) noexcept;
  static void Unref(Shard& shard, Entry& entry, Entry*& retired) noexcept;
  static void FinishErase(Shard& shard, Entry* entry, Entry*& retired) noexcept;
  static void Evict(Shard& shard, Entry*& retired) noexcept;
  static void Retire(Entry& entry, Entry*& retired) noexcept;
  static void DeleteRetired(Entry* retired) noexcept;
  void Initialize(std::size_t capacity) noexcept;
  void Destroy() noexcept;
  void Release(Entry& entry) noexcept;

  std::array<Shard, ShardCount> shards_;
  std::atomic<std::uint64_t> next_id_{0};
};

}  // namespace modern_leveldb

#endif  // MODERN_LEVELDB_CACHE_SHARDED_LRU_CACHE_CORE_H_
