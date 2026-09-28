#ifndef MODERN_LEVELDB_CACHE_SHARDED_LRU_CACHE_H_
#define MODERN_LEVELDB_CACHE_SHARDED_LRU_CACHE_H_

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

#include "modern_leveldb/base/bytes.h"
#include "modern_leveldb/base/hash.h"
#include "modern_leveldb/base/result.h"

namespace modern_leveldb {

template <typename Value>
class ShardedLruCache final {
 private:
  struct Entry;
  struct Shard;

 public:
  class Handle final {
   public:
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;

    Handle(Handle&& source) noexcept
        : shard_(std::exchange(source.shard_, nullptr)),
          entry_(std::exchange(source.entry_, nullptr)) {}

    Handle& operator=(Handle&& source) noexcept {
      if (this != &source) {
        Reset();
        shard_ = std::exchange(source.shard_, nullptr);
        entry_ = std::exchange(source.entry_, nullptr);
      }
      return *this;
    }

    ~Handle() { Reset(); }

    [[nodiscard]] const Value& value() const noexcept {
      assert(entry_ != nullptr);
      return *entry_->value;
    }
    [[nodiscard]] const Value* operator->() const noexcept { return &value(); }
    [[nodiscard]] const Value& operator*() const noexcept { return value(); }

   private:
    friend class ShardedLruCache;

    Handle(Shard& shard, Entry& entry) noexcept : shard_(&shard), entry_(&entry) {}

    void Reset() noexcept {
      if (entry_ != nullptr) {
        ShardedLruCache::Release(*shard_, *entry_);
        shard_ = nullptr;
        entry_ = nullptr;
      }
    }

    Shard* shard_ = nullptr;
    Entry* entry_ = nullptr;
  };

  explicit ShardedLruCache(std::size_t capacity) {
    const std::size_t shard_capacity =
        capacity / ShardCount + (capacity % ShardCount != 0U ? 1U : 0U);
    for (Shard& shard : shards_) {
      shard.capacity = shard_capacity;
    }
  }

  ShardedLruCache(const ShardedLruCache&) = delete;
  ShardedLruCache& operator=(const ShardedLruCache&) = delete;
  ShardedLruCache(ShardedLruCache&&) = delete;
  ShardedLruCache& operator=(ShardedLruCache&&) = delete;

  ~ShardedLruCache() {
    for (Shard& shard : shards_) {
      assert(shard.in_use.next == &shard.in_use);
      while (shard.lru.next != &shard.lru) {
        Entry* entry = shard.lru.next;
        Entry* removed = shard.table.Remove(entry->key, entry->hash);
        assert(removed == entry);
        (void)removed;
        FinishErase(shard, *entry);
        assert(entry->refs == 0);
        delete entry;
      }
      assert(shard.usage == 0);
    }
  }

  [[nodiscard]] Result<Handle> Insert(ByteView key, std::shared_ptr<const Value> value,
                                      std::size_t charge) {
    if (value == nullptr) {
      return std::unexpected(Error::InvalidArgument("cache value is null"));
    }
    if (charge == 0) {
      return std::unexpected(Error::InvalidArgument("cache charge must be positive"));
    }

    auto entry = std::make_unique<Entry>(std::vector<std::byte>(key.begin(), key.end()),
                                         std::move(value), charge, Hash32(key, 0U));
    Shard& shard = shards_[ShardIndex(entry->hash)];
    std::vector<Entry*> retired;
    {
      std::lock_guard lock(shard.mutex);
      Entry* existing = shard.table.Lookup(entry->key, entry->hash);
      const std::size_t base_charge = shard.usage - (existing == nullptr ? 0U : existing->charge);
      if (charge > std::numeric_limits<std::size_t>::max() - base_charge) {
        return std::unexpected(Error::InvalidArgument("cache charge accounting overflow"));
      }

      retired.reserve(shard.table.size() + 1U);
      if (shard.capacity != 0) {
        entry->refs = 2;
        entry->in_cache = true;
        Entry* replaced = shard.table.Insert(entry.get());
        Append(shard.in_use, *entry);
        if (replaced != nullptr) {
          FinishErase(shard, *replaced);
          if (replaced->refs == 0) {
            retired.push_back(replaced);
          }
        }
        assert(shard.usage == base_charge);
        shard.usage += charge;
        Evict(shard, retired);
      }
    }

    for (Entry* removed : retired) {
      delete removed;
    }
    Entry* inserted = entry.release();
    return Handle(shard, *inserted);
  }

  [[nodiscard]] std::optional<Handle> Lookup(ByteView key) {
    const std::uint32_t hash = Hash32(key, 0U);
    Shard& shard = shards_[ShardIndex(hash)];
    std::lock_guard lock(shard.mutex);
    Entry* entry = shard.table.Lookup(key, hash);
    if (entry == nullptr) {
      return std::nullopt;
    }
    Ref(shard, *entry);
    return Handle(shard, *entry);
  }

  void Erase(ByteView key) {
    const std::uint32_t hash = Hash32(key, 0U);
    Shard& shard = shards_[ShardIndex(hash)];
    Entry* retired = nullptr;
    {
      std::lock_guard lock(shard.mutex);
      Entry* entry = shard.table.Remove(key, hash);
      if (entry == nullptr) {
        return;
      }
      FinishErase(shard, *entry);
      if (entry->refs == 0) {
        retired = entry;
      }
    }
    delete retired;
  }

  [[nodiscard]] std::size_t total_charge() const {
    std::size_t total = 0;
    for (const Shard& shard : shards_) {
      std::lock_guard lock(shard.mutex);
      if (shard.usage > std::numeric_limits<std::size_t>::max() - total) {
        return std::numeric_limits<std::size_t>::max();
      }
      total += shard.usage;
    }
    return total;
  }

  [[nodiscard]] std::uint64_t NewId() noexcept {
    return next_id_.fetch_add(1U, std::memory_order_relaxed) + 1U;
  }

 private:
  static constexpr std::size_t ShardCount = 16;
  static constexpr unsigned int ShardBits = 4;

  struct Entry {
    Entry() noexcept : next(this), previous(this) {}

    Entry(std::vector<std::byte> entry_key, std::shared_ptr<const Value> entry_value,
          std::size_t entry_charge, std::uint32_t entry_hash)
        : value(std::move(entry_value)),
          key(std::move(entry_key)),
          charge(entry_charge),
          hash(entry_hash) {}

    std::shared_ptr<const Value> value;
    std::vector<std::byte> key;
    std::size_t charge = 0;
    std::uint32_t hash = 0;
    std::size_t refs = 1;
    bool in_cache = false;
    Entry* next_hash = nullptr;
    Entry* next = nullptr;
    Entry* previous = nullptr;
  };

  class HandleTable final {
   public:
    HandleTable() : buckets_(4, nullptr) {}

    [[nodiscard]] std::size_t size() const noexcept { return elements_; }

    [[nodiscard]] Entry* Lookup(ByteView key, std::uint32_t hash) const noexcept {
      Entry* entry = buckets_[hash & static_cast<std::uint32_t>(buckets_.size() - 1U)];
      while (entry != nullptr && (entry->hash != hash || !std::ranges::equal(entry->key, key))) {
        entry = entry->next_hash;
      }
      return entry;
    }

    Entry* Insert(Entry* entry) {
      if (elements_ >= buckets_.size()) {
        Resize();
      }
      Entry** position = FindPointer(entry->key, entry->hash);
      Entry* replaced = *position;
      if (replaced == nullptr) {
        ++elements_;
      }
      entry->next_hash = replaced == nullptr ? nullptr : replaced->next_hash;
      *position = entry;
      return replaced;
    }

    Entry* Remove(ByteView key, std::uint32_t hash) noexcept {
      Entry** position = FindPointer(key, hash);
      Entry* removed = *position;
      if (removed != nullptr) {
        *position = removed->next_hash;
        removed->next_hash = nullptr;
        --elements_;
      }
      return removed;
    }

   private:
    [[nodiscard]] Entry** FindPointer(ByteView key, std::uint32_t hash) noexcept {
      Entry** position = &buckets_[hash & static_cast<std::uint32_t>(buckets_.size() - 1U)];
      while (*position != nullptr &&
             ((*position)->hash != hash || !std::ranges::equal((*position)->key, key))) {
        position = &(*position)->next_hash;
      }
      return position;
    }

    void Resize() {
      const std::size_t length = buckets_.size() * 2U;
      std::vector<Entry*> buckets(length, nullptr);
      for (Entry* head : buckets_) {
        while (head != nullptr) {
          Entry* next = head->next_hash;
          const std::size_t index = head->hash & static_cast<std::uint32_t>(buckets.size() - 1U);
          head->next_hash = buckets[index];
          buckets[index] = head;
          head = next;
        }
      }
      buckets_ = std::move(buckets);
    }

    std::vector<Entry*> buckets_;
    std::size_t elements_ = 0;
  };

  struct Shard {
    Shard() = default;

    mutable std::mutex mutex;
    std::size_t capacity = 0;
    std::size_t usage = 0;
    HandleTable table;
    Entry lru;
    Entry in_use;
  };

  [[nodiscard]] static std::size_t ShardIndex(std::uint32_t hash) noexcept {
    return hash >> (32U - ShardBits);
  }

  static void Remove(Entry& entry) noexcept {
    assert(entry.next != nullptr && entry.previous != nullptr);
    entry.previous->next = entry.next;
    entry.next->previous = entry.previous;
    entry.next = nullptr;
    entry.previous = nullptr;
  }

  static void Append(Entry& list, Entry& entry) noexcept {
    assert(entry.next == nullptr && entry.previous == nullptr);
    entry.next = &list;
    entry.previous = list.previous;
    list.previous->next = &entry;
    list.previous = &entry;
  }

  static void Ref(Shard& shard, Entry& entry) noexcept {
    assert(entry.refs > 0);
    if (entry.refs == 1 && entry.in_cache) {
      Remove(entry);
      Append(shard.in_use, entry);
    }
    ++entry.refs;
  }

  static void Release(Shard& shard, Entry& entry) noexcept {
    Entry* retired = nullptr;
    {
      std::lock_guard lock(shard.mutex);
      assert(entry.refs > 0);
      --entry.refs;
      if (entry.refs == 0) {
        assert(!entry.in_cache);
        retired = &entry;
      } else if (entry.refs == 1 && entry.in_cache) {
        Remove(entry);
        Append(shard.lru, entry);
      }
    }
    delete retired;
  }

  static void FinishErase(Shard& shard, Entry& entry) noexcept {
    assert(entry.in_cache);
    Remove(entry);
    entry.in_cache = false;
    assert(shard.usage >= entry.charge);
    shard.usage -= entry.charge;
    assert(entry.refs > 0);
    --entry.refs;
  }

  static void Evict(Shard& shard, std::vector<Entry*>& retired) noexcept {
    while (shard.usage > shard.capacity && shard.lru.next != &shard.lru) {
      Entry* entry = shard.lru.next;
      assert(entry->refs == 1 && entry->in_cache);
      Entry* removed = shard.table.Remove(entry->key, entry->hash);
      assert(removed == entry);
      (void)removed;
      FinishErase(shard, *entry);
      assert(entry->refs == 0);
      retired.push_back(entry);
    }
  }

  std::array<Shard, ShardCount> shards_;
  std::atomic<std::uint64_t> next_id_{0};
};

}  // namespace modern_leveldb

#endif  // MODERN_LEVELDB_CACHE_SHARDED_LRU_CACHE_H_
