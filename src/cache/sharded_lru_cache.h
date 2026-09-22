#ifndef MODERN_LEVELDB_CACHE_SHARDED_LRU_CACHE_H_
#define MODERN_LEVELDB_CACHE_SHARDED_LRU_CACHE_H_

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <limits>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
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

 public:
  class Handle final {
   public:
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&&) noexcept = default;
    Handle& operator=(Handle&&) noexcept = default;
    ~Handle() = default;

    [[nodiscard]] const Value& value() const noexcept { return *entry_->value; }
    [[nodiscard]] const Value* operator->() const noexcept { return entry_->value.get(); }
    [[nodiscard]] const Value& operator*() const noexcept { return *entry_->value; }

   private:
    friend class ShardedLruCache;

    explicit Handle(std::shared_ptr<Entry> entry) : entry_(std::move(entry)) {}

    std::shared_ptr<Entry> entry_;
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
  ~ShardedLruCache() = default;

  [[nodiscard]] Result<Handle> Insert(ByteView key, std::shared_ptr<const Value> value,
                                      std::size_t charge) {
    if (value == nullptr) {
      return std::unexpected(Error::InvalidArgument("cache value is null"));
    }
    if (charge == 0) {
      return std::unexpected(Error::InvalidArgument("cache charge must be positive"));
    }

    auto entry = std::make_shared<Entry>(std::vector<std::byte>(key.begin(), key.end()),
                                         std::move(value), charge);
    Shard& shard = shards_[ShardIndex(key)];
    if (shard.capacity == 0) {
      return Handle(std::move(entry));
    }

    std::vector<std::shared_ptr<Entry>> retired;
    bool overflow = false;
    {
      std::lock_guard lock(shard.mutex);
      retired.reserve(shard.entries.size() + 1U);
      auto existing = shard.entries.find(key);
      const std::size_t base_charge =
          shard.usage - (existing == shard.entries.end() ? 0U : existing->second->charge);
      if (charge > std::numeric_limits<std::size_t>::max() - base_charge) {
        overflow = true;
      } else if (existing != shard.entries.end()) {
        shard.lru.push_back(entry.get());
        entry->lru_position = std::prev(shard.lru.end());

        retired.push_back(std::move(existing->second));
        shard.lru.erase(retired.back()->lru_position);
        existing->second = entry;
        shard.usage = base_charge + charge;
        Evict(shard, retired);
      } else {
        const auto [inserted, was_inserted] = shard.entries.emplace(entry->key, entry);
        (void)was_inserted;
        try {
          shard.lru.push_back(entry.get());
        } catch (...) {
          shard.entries.erase(inserted);
          throw;
        }
        entry->lru_position = std::prev(shard.lru.end());
        shard.usage = base_charge + charge;
        Evict(shard, retired);
      }
    }

    if (overflow) {
      return std::unexpected(Error::InvalidArgument("cache charge accounting overflow"));
    }
    return Handle(std::move(entry));
  }

  [[nodiscard]] std::optional<Handle> Lookup(ByteView key) {
    Shard& shard = shards_[ShardIndex(key)];
    std::shared_ptr<Entry> entry;
    {
      std::lock_guard lock(shard.mutex);
      const auto found = shard.entries.find(key);
      if (found == shard.entries.end()) {
        return std::nullopt;
      }
      entry = found->second;
      shard.lru.splice(shard.lru.end(), shard.lru, entry->lru_position);
    }
    return Handle(std::move(entry));
  }

  void Erase(ByteView key) {
    Shard& shard = shards_[ShardIndex(key)];
    std::shared_ptr<Entry> retired;
    {
      std::lock_guard lock(shard.mutex);
      const auto found = shard.entries.find(key);
      if (found == shard.entries.end()) {
        return;
      }
      retired = std::move(found->second);
      shard.lru.erase(retired->lru_position);
      shard.usage -= retired->charge;
      shard.entries.erase(found);
    }
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

  struct KeyHash {
    using is_transparent = void;

    [[nodiscard]] std::size_t operator()(ByteView key) const noexcept { return Hash32(key, 0U); }
    [[nodiscard]] std::size_t operator()(const std::vector<std::byte>& key) const noexcept {
      return (*this)(ByteView(key));
    }
  };

  struct KeyEqual {
    using is_transparent = void;

    [[nodiscard]] bool operator()(ByteView left, ByteView right) const noexcept {
      return std::ranges::equal(left, right);
    }
    [[nodiscard]] bool operator()(const std::vector<std::byte>& left,
                                  const std::vector<std::byte>& right) const noexcept {
      return (*this)(ByteView(left), ByteView(right));
    }
    [[nodiscard]] bool operator()(const std::vector<std::byte>& left,
                                  ByteView right) const noexcept {
      return (*this)(ByteView(left), right);
    }
    [[nodiscard]] bool operator()(ByteView left,
                                  const std::vector<std::byte>& right) const noexcept {
      return (*this)(left, ByteView(right));
    }
  };

  using EntryMap =
      std::unordered_map<std::vector<std::byte>, std::shared_ptr<Entry>, KeyHash, KeyEqual>;

  struct Entry {
    Entry(std::vector<std::byte> entry_key, std::shared_ptr<const Value> entry_value,
          std::size_t entry_charge)
        : key(std::move(entry_key)), value(std::move(entry_value)), charge(entry_charge) {}

    std::vector<std::byte> key;
    std::shared_ptr<const Value> value;
    std::size_t charge;
    typename std::list<Entry*>::iterator lru_position;
  };

  struct Shard {
    mutable std::mutex mutex;
    std::size_t capacity = 0;
    std::size_t usage = 0;
    EntryMap entries;
    std::list<Entry*> lru;
  };

  [[nodiscard]] static std::size_t ShardIndex(ByteView key) noexcept {
    return Hash32(key, 0U) >> (32U - ShardBits);
  }

  static void Evict(Shard& shard, std::vector<std::shared_ptr<Entry>>& retired) {
    auto position = shard.lru.begin();
    while (shard.usage > shard.capacity && position != shard.lru.end()) {
      Entry* candidate = *position;
      const auto next = std::next(position);
      const auto found = shard.entries.find(candidate->key);
      if (found != shard.entries.end() && found->second.use_count() == 1) {
        retired.push_back(std::move(found->second));
        shard.usage -= candidate->charge;
        shard.lru.erase(position);
        shard.entries.erase(found);
      }
      position = next;
    }
  }

  std::array<Shard, ShardCount> shards_;
  std::atomic<std::uint64_t> next_id_{0};
};

}  // namespace modern_leveldb

#endif  // MODERN_LEVELDB_CACHE_SHARDED_LRU_CACHE_H_
