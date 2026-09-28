#include "cache/intrusive_lru_cache.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <utility>
#include <vector>

#include "modern_leveldb/base/hash.h"

namespace modern_leveldb {
namespace {

constexpr std::size_t ShardCount = 16;
constexpr unsigned int ShardBits = 4;

}  // namespace

class IntrusiveLruCache::Implementation final {
 private:
  class SentinelEntry final : public Entry {
   public:
    SentinelEntry() : Entry({}, 0, false) {}
  };

  class HandleTable final {
   public:
    HandleTable() : buckets_(4, nullptr) {}

    [[nodiscard]] std::size_t size() const noexcept { return elements_; }

    [[nodiscard]] Entry* Lookup(ByteView key, std::uint32_t hash) const noexcept {
      Entry* entry = buckets_[hash & static_cast<std::uint32_t>(buckets_.size() - 1U)];
      while (entry != nullptr && (entry->hash_ != hash || !std::ranges::equal(entry->key_, key))) {
        entry = entry->next_hash_;
      }
      return entry;
    }

    Entry* Insert(Entry* entry) {
      if (elements_ >= buckets_.size()) {
        Resize();
      }
      Entry** position = FindPointer(entry->key_, entry->hash_);
      Entry* replaced = *position;
      if (replaced == nullptr) {
        ++elements_;
      }
      entry->next_hash_ = replaced == nullptr ? nullptr : replaced->next_hash_;
      *position = entry;
      return replaced;
    }

    Entry* Remove(ByteView key, std::uint32_t hash) noexcept {
      Entry** position = FindPointer(key, hash);
      Entry* removed = *position;
      if (removed != nullptr) {
        *position = removed->next_hash_;
        removed->next_hash_ = nullptr;
        --elements_;
      }
      return removed;
    }

   private:
    [[nodiscard]] Entry** FindPointer(ByteView key, std::uint32_t hash) noexcept {
      Entry** position = &buckets_[hash & static_cast<std::uint32_t>(buckets_.size() - 1U)];
      while (*position != nullptr &&
             ((*position)->hash_ != hash || !std::ranges::equal((*position)->key_, key))) {
        position = &(*position)->next_hash_;
      }
      return position;
    }

    void Resize() {
      const std::size_t length = buckets_.size() * 2U;
      std::vector<Entry*> buckets(length, nullptr);
      for (Entry* head : buckets_) {
        while (head != nullptr) {
          Entry* next = head->next_hash_;
          const std::size_t index = head->hash_ & static_cast<std::uint32_t>(buckets.size() - 1U);
          head->next_hash_ = buckets[index];
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
    Shard() {
      lru.next_ = lru.previous_ = &lru;
      in_use.next_ = in_use.previous_ = &in_use;
    }

    mutable std::mutex mutex;
    std::size_t capacity = 0;
    std::size_t usage = 0;
    HandleTable table;
    SentinelEntry lru;
    SentinelEntry in_use;
  };

 public:
  explicit Implementation(std::size_t capacity) {
    const std::size_t shard_capacity =
        capacity / ShardCount + (capacity % ShardCount != 0U ? 1U : 0U);
    for (Shard& shard : shards_) {
      shard.capacity = shard_capacity;
    }
  }

  ~Implementation() {
    for (Shard& shard : shards_) {
      assert(shard.in_use.next_ == &shard.in_use);
      while (shard.lru.next_ != &shard.lru) {
        Entry* entry = shard.lru.next_;
        Entry* removed = shard.table.Remove(entry->key_, entry->hash_);
        assert(removed == entry);
        (void)removed;
        FinishErase(shard, *entry);
        assert(entry->refs_ == 0);
        Delete(entry);
      }
      assert(shard.usage == 0);
    }
  }

  [[nodiscard]] Result<Pin> Insert(std::unique_ptr<Entry> entry) {
    if (!entry->has_value_) {
      return std::unexpected(Error::InvalidArgument("cache value is null"));
    }
    if (entry->charge_ == 0) {
      return std::unexpected(Error::InvalidArgument("cache charge must be positive"));
    }

    Shard& shard = shards_[ShardIndex(entry->hash_)];
    std::vector<Entry*> retired;
    {
      std::lock_guard lock(shard.mutex);
      Entry* existing = shard.table.Lookup(entry->key_, entry->hash_);
      const std::size_t base_charge = shard.usage - (existing == nullptr ? 0U : existing->charge_);
      if (entry->charge_ > std::numeric_limits<std::size_t>::max() - base_charge) {
        return std::unexpected(Error::InvalidArgument("cache charge accounting overflow"));
      }

      retired.reserve(shard.table.size() + 1U);
      if (shard.capacity != 0) {
        entry->refs_ = 2;
        entry->in_cache_ = true;
        Entry* replaced = shard.table.Insert(entry.get());
        Append(shard.in_use, *entry);
        if (replaced != nullptr) {
          FinishErase(shard, *replaced);
          if (replaced->refs_ == 0) {
            retired.push_back(replaced);
          }
        }
        assert(shard.usage == base_charge);
        shard.usage += entry->charge_;
        Evict(shard, retired);
      }
    }

    for (Entry* removed : retired) {
      Delete(removed);
    }
    Entry* inserted = entry.release();
    return Pin(*this, *inserted);
  }

  [[nodiscard]] std::optional<Pin> Lookup(ByteView key) {
    const std::uint32_t hash = Hash32(key, 0U);
    Shard& shard = shards_[ShardIndex(hash)];
    std::lock_guard lock(shard.mutex);
    Entry* entry = shard.table.Lookup(key, hash);
    if (entry == nullptr) {
      return std::nullopt;
    }
    Ref(shard, *entry);
    return Pin(*this, *entry);
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
      if (entry->refs_ == 0) {
        retired = entry;
      }
    }
    Delete(retired);
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

  void Release(Entry& entry) noexcept {
    Shard& shard = shards_[ShardIndex(entry.hash_)];
    Entry* retired = nullptr;
    {
      std::lock_guard lock(shard.mutex);
      assert(entry.refs_ > 0);
      --entry.refs_;
      if (entry.refs_ == 0) {
        assert(!entry.in_cache_);
        retired = &entry;
      } else if (entry.refs_ == 1 && entry.in_cache_) {
        Remove(entry);
        Append(shard.lru, entry);
      }
    }
    Delete(retired);
  }

 private:
  [[nodiscard]] static std::size_t ShardIndex(std::uint32_t hash) noexcept {
    return hash >> (32U - ShardBits);
  }

  static void Remove(Entry& entry) noexcept {
    assert(entry.next_ != nullptr && entry.previous_ != nullptr);
    entry.previous_->next_ = entry.next_;
    entry.next_->previous_ = entry.previous_;
    entry.next_ = nullptr;
    entry.previous_ = nullptr;
  }

  static void Append(Entry& list, Entry& entry) noexcept {
    assert(entry.next_ == nullptr && entry.previous_ == nullptr);
    entry.next_ = &list;
    entry.previous_ = list.previous_;
    list.previous_->next_ = &entry;
    list.previous_ = &entry;
  }

  static void Ref(Shard& shard, Entry& entry) noexcept {
    assert(entry.refs_ > 0);
    if (entry.refs_ == 1) {
      assert(entry.in_cache_);
      Remove(entry);
      Append(shard.in_use, entry);
    }
    ++entry.refs_;
  }

  static void FinishErase(Shard& shard, Entry& entry) noexcept {
    assert(entry.in_cache_);
    Remove(entry);
    entry.in_cache_ = false;
    assert(shard.usage >= entry.charge_);
    shard.usage -= entry.charge_;
    assert(entry.refs_ > 0);
    --entry.refs_;
  }

  static void Evict(Shard& shard, std::vector<Entry*>& retired) noexcept {
    while (shard.usage > shard.capacity && shard.lru.next_ != &shard.lru) {
      Entry* entry = shard.lru.next_;
      assert(entry->refs_ == 1 && entry->in_cache_);
      Entry* removed = shard.table.Remove(entry->key_, entry->hash_);
      assert(removed == entry);
      (void)removed;
      FinishErase(shard, *entry);
      assert(entry->refs_ == 0);
      retired.push_back(entry);
    }
  }

  static void Delete(Entry* entry) noexcept { delete entry; }

  std::array<Shard, ShardCount> shards_;
  std::atomic<std::uint64_t> next_id_{0};
};

// GCOVR_EXCL_START: GCC emits duplicate constructor/destructor ABI clones
IntrusiveLruCache::Entry::Entry(std::vector<std::byte> key, std::size_t charge, bool has_value)
    : key_(std::move(key)), charge_(charge), hash_(Hash32(key_, 0U)), has_value_(has_value) {}

IntrusiveLruCache::Entry::~Entry() = default;

IntrusiveLruCache::Pin::Pin(Implementation& implementation, Entry& entry) noexcept
    : implementation_(&implementation), entry_(&entry) {}

IntrusiveLruCache::Pin::Pin(Pin&& source) noexcept
    : implementation_(std::exchange(source.implementation_, nullptr)),
      entry_(std::exchange(source.entry_, nullptr)) {}

IntrusiveLruCache::Pin& IntrusiveLruCache::Pin::operator=(Pin&& source) noexcept {
  if (this != &source) {
    Reset();
    implementation_ = std::exchange(source.implementation_, nullptr);
    entry_ = std::exchange(source.entry_, nullptr);
  }
  return *this;
}

IntrusiveLruCache::Pin::~Pin() { Reset(); }
// GCOVR_EXCL_STOP

const IntrusiveLruCache::Entry& IntrusiveLruCache::Pin::entry() const noexcept {
  assert(entry_ != nullptr);
  return *entry_;
}

void IntrusiveLruCache::Pin::Reset() noexcept {
  if (entry_ != nullptr) {
    implementation_->Release(*entry_);
    implementation_ = nullptr;
    entry_ = nullptr;
  }
}

// GCOVR_EXCL_START: GCC emits duplicate constructor/destructor ABI clones
IntrusiveLruCache::IntrusiveLruCache(std::size_t capacity)
    : implementation_(std::make_unique<Implementation>(capacity)) {}

IntrusiveLruCache::~IntrusiveLruCache() = default;
// GCOVR_EXCL_STOP

Result<IntrusiveLruCache::Pin> IntrusiveLruCache::Insert(std::unique_ptr<Entry> entry) {
  return implementation_->Insert(std::move(entry));
}

std::optional<IntrusiveLruCache::Pin> IntrusiveLruCache::Lookup(ByteView key) {
  return implementation_->Lookup(key);
}

void IntrusiveLruCache::Erase(ByteView key) { implementation_->Erase(key); }

std::size_t IntrusiveLruCache::total_charge() const { return implementation_->total_charge(); }

std::uint64_t IntrusiveLruCache::NewId() noexcept { return implementation_->NewId(); }

}  // namespace modern_leveldb
