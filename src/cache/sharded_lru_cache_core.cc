#include "cache/sharded_lru_cache_core.h"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <new>
#include <utility>

#include "modern_leveldb/base/hash.h"

namespace modern_leveldb {

ByteView ShardedLruCacheCore::Entry::key() const noexcept {
  assert(next != this);
  const auto* bytes = reinterpret_cast<const std::byte*>(this) + sizeof(Entry);
  return ByteView(bytes, key_length);
}

// GCOVR_EXCL_START: GCC emits duplicate constructor/destructor ABI clones
ShardedLruCacheCore::Pin::Pin(ShardedLruCacheCore& cache, Entry& entry) noexcept
    : cache_(&cache), entry_(&entry) {}

ShardedLruCacheCore::Pin::Pin(Pin&& source) noexcept
    : cache_(std::exchange(source.cache_, nullptr)),
      entry_(std::exchange(source.entry_, nullptr)) {}

ShardedLruCacheCore::Pin& ShardedLruCacheCore::Pin::operator=(Pin&& source) noexcept {
  if (this != &source) {
    Reset();
    cache_ = std::exchange(source.cache_, nullptr);
    entry_ = std::exchange(source.entry_, nullptr);
  }
  return *this;
}

ShardedLruCacheCore::Pin::~Pin() { Reset(); }
// GCOVR_EXCL_STOP

const void* ShardedLruCacheCore::Pin::value() const noexcept {
  assert(entry_ != nullptr);
  return entry_->value;
}

void ShardedLruCacheCore::Pin::Reset() noexcept {
  if (entry_ != nullptr) {
    cache_->Release(*entry_);
    cache_ = nullptr;
    entry_ = nullptr;
  }
}

// GCOVR_EXCL_START: GCC emits duplicate constructor ABI clones
ShardedLruCacheCore::HandleTable::HandleTable() : length_(4), buckets_(AllocateBuckets(length_)) {}
// GCOVR_EXCL_STOP

ShardedLruCacheCore::Entry* ShardedLruCacheCore::HandleTable::Lookup(
    ByteView key, std::uint32_t hash) const noexcept {
  Entry* entry = buckets_[hash & (length_ - 1U)];
  while (entry != nullptr && (entry->hash != hash || !std::ranges::equal(entry->key(), key))) {
    entry = entry->next_hash;
  }
  return entry;
}

ShardedLruCacheCore::HandleTable::PreparedInsert ShardedLruCacheCore::HandleTable::PrepareInsert(
    ByteView key, std::uint32_t hash) noexcept {
  Entry** position = FindPointer(key, hash);
  Entry* existing = *position;
  return PreparedInsert{
      .position = position,
      .existing = existing,
      .needs_growth = existing == nullptr && elements_ == length_,
  };
}

ShardedLruCacheCore::Entry* ShardedLruCacheCore::HandleTable::CommitInsert(PreparedInsert prepared,
                                                                           Entry* entry) {
  std::unique_ptr<Entry*[]> growth;
  const std::uint32_t growth_length = prepared.needs_growth ? length_ * 2U : 0U;
  if (prepared.needs_growth) {
    growth = AllocateBuckets(growth_length);
  }

  Entry* replaced = prepared.existing;
  entry->next_hash = replaced == nullptr ? nullptr : replaced->next_hash;
  *prepared.position = entry;
  if (replaced == nullptr) {
    ++elements_;
  }

  if (prepared.needs_growth) {
    RehashInto(std::move(growth), growth_length);
  }
  return replaced;
}

ShardedLruCacheCore::Entry* ShardedLruCacheCore::HandleTable::Remove(ByteView key,
                                                                     std::uint32_t hash) noexcept {
  Entry** position = FindPointer(key, hash);
  Entry* removed = *position;
  if (removed != nullptr) {
    *position = removed->next_hash;
    removed->next_hash = nullptr;
    --elements_;
  }
  return removed;
}

ShardedLruCacheCore::Entry** ShardedLruCacheCore::HandleTable::FindPointer(
    ByteView key, std::uint32_t hash) noexcept {
  Entry** position = &buckets_[hash & (length_ - 1U)];
  while (*position != nullptr &&
         ((*position)->hash != hash || !std::ranges::equal((*position)->key(), key))) {
    position = &(*position)->next_hash;
  }
  return position;
}

std::unique_ptr<ShardedLruCacheCore::Entry*[]> ShardedLruCacheCore::HandleTable::AllocateBuckets(
    std::uint32_t length) {
  auto buckets = std::make_unique<Entry*[]>(length);
  for (std::uint32_t index = 0; index < length; ++index) {
    buckets[index] = nullptr;
  }
  return buckets;
}

void ShardedLruCacheCore::HandleTable::RehashInto(std::unique_ptr<Entry*[]> buckets,
                                                  std::uint32_t length) noexcept {
  [[maybe_unused]] std::uint32_t count = 0;
  for (std::uint32_t index = 0; index < length_; ++index) {
    Entry* entry = buckets_[index];
    while (entry != nullptr) {
      Entry* next = entry->next_hash;
      Entry** position = &buckets[entry->hash & (length - 1U)];
      entry->next_hash = *position;
      *position = entry;
      entry = next;
      ++count;
    }
  }
  assert(count == elements_);
  buckets_ = std::move(buckets);
  length_ = length;
}

// GCOVR_EXCL_START: GCC emits duplicate constructor ABI clones
ShardedLruCacheCore::Shard::Shard()
    : lru(nullptr, nullptr, 0, 0, 0), in_use(nullptr, nullptr, 0, 0, 0) {
  lru.next = lru.previous = &lru;
  in_use.next = in_use.previous = &in_use;
}
// GCOVR_EXCL_STOP

void ShardedLruCacheCore::ErasedValueDeleter::operator()(const void* value) const noexcept {
  assert(value != nullptr);
  deleter(value);
}

// GCOVR_EXCL_START: GCC emits duplicate constructor/destructor ABI clones
ShardedLruCacheCore::ShardedLruCacheCore(std::size_t capacity) { Initialize(capacity); }

ShardedLruCacheCore::~ShardedLruCacheCore() { Destroy(); }
// GCOVR_EXCL_STOP

void ShardedLruCacheCore::Initialize(std::size_t capacity) noexcept {
  const std::size_t shard_capacity =
      capacity / ShardCount + (capacity % ShardCount != 0U ? 1U : 0U);
  for (Shard& shard : shards_) {
    shard.capacity = shard_capacity;
  }
}

void ShardedLruCacheCore::Destroy() noexcept {
  Entry* retired = nullptr;
  for (Shard& shard : shards_) {
    assert(shard.in_use.next == &shard.in_use);
    for (Entry* entry = shard.lru.next; entry != &shard.lru;) {
      Entry* next = entry->next;
      assert(entry->in_cache && entry->refs == 1);
      entry->in_cache = false;
      Unref(shard, *entry, retired);
      entry = next;
    }
    shard.usage = 0;
  }
  DeleteRetired(retired);
}

ShardedLruCacheCore::Pin ShardedLruCacheCore::Insert(ByteView key, const void* value,
                                                     ValueDeleter deleter, std::size_t charge) {
  PendingValue pending_value(value, ErasedValueDeleter{deleter});
  PendingEntry entry = CreateEntry(key, std::move(pending_value), charge);
  Shard& shard = shards_[ShardIndex(entry->hash)];
  Entry* retired = nullptr;
  {
    std::lock_guard lock(shard.mutex);
    HandleTable::PreparedInsert prepared = shard.table.PrepareInsert(entry->key(), entry->hash);
    const std::size_t base_charge =
        shard.usage - (prepared.existing == nullptr ? 0U : prepared.existing->charge);
    const bool charge_fits = entry->charge <= std::numeric_limits<std::size_t>::max() - base_charge;
    if (shard.capacity != 0 && charge_fits) {
      Entry* replaced = shard.table.CommitInsert(prepared, entry.get());
      entry->refs = 2;
      entry->in_cache = true;
      Append(shard.in_use, *entry);
      FinishErase(shard, replaced, retired);
      assert(shard.usage == base_charge);
      shard.usage += entry->charge;
      Evict(shard, retired);
    }
  }
  DeleteRetired(retired);
  Entry* inserted = entry.release();
  return Pin(*this, *inserted);
}

std::optional<ShardedLruCacheCore::Pin> ShardedLruCacheCore::Lookup(ByteView key) {
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

void ShardedLruCacheCore::Erase(ByteView key) {
  const std::uint32_t hash = Hash32(key, 0U);
  Shard& shard = shards_[ShardIndex(hash)];
  Entry* retired = nullptr;
  {
    std::lock_guard lock(shard.mutex);
    FinishErase(shard, shard.table.Remove(key, hash), retired);
  }
  DeleteRetired(retired);
}

std::size_t ShardedLruCacheCore::total_charge() const {
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

std::uint64_t ShardedLruCacheCore::NewId() noexcept {
  return next_id_.fetch_add(1U, std::memory_order_relaxed) + 1U;
}

std::size_t ShardedLruCacheCore::ShardIndex(std::uint32_t hash) noexcept {
  return hash >> (32U - ShardBits);
}

ShardedLruCacheCore::PendingEntry ShardedLruCacheCore::CreateEntry(ByteView key, PendingValue value,
                                                                   std::size_t charge) {
  // GCOVR_EXCL_START: requires a ByteView larger than SIZE_MAX - sizeof(Entry)
  if (key.size() > std::numeric_limits<std::size_t>::max() - sizeof(Entry)) {
    throw std::bad_array_new_length();
  }
  // GCOVR_EXCL_STOP
  void* storage = ::operator new(sizeof(Entry) + key.size());
  auto* entry = static_cast<Entry*>(storage);
  std::construct_at(entry, value.get(), value.get_deleter().deleter, charge, key.size(),
                    Hash32(key, 0U));
  PendingEntry owned(entry, &DeleteEntry);
  static_cast<void>(value.release());
  std::ranges::copy(key, reinterpret_cast<std::byte*>(entry) + sizeof(Entry));
  return owned;
}

void ShardedLruCacheCore::DeleteEntry(Entry* entry) noexcept {
  assert(entry != nullptr);
  entry->deleter(entry->value);
  entry->~Entry();
  ::operator delete(entry);
}

void ShardedLruCacheCore::Remove(Entry& entry) noexcept {
  assert(entry.next != nullptr && entry.previous != nullptr);
  entry.next->previous = entry.previous;
  entry.previous->next = entry.next;
  entry.next = nullptr;
  entry.previous = nullptr;
}

void ShardedLruCacheCore::Append(Entry& list, Entry& entry) noexcept {
  assert(entry.next == nullptr && entry.previous == nullptr);
  entry.next = &list;
  entry.previous = list.previous;
  entry.previous->next = &entry;
  entry.next->previous = &entry;
}

void ShardedLruCacheCore::Ref(Shard& shard, Entry& entry) noexcept {
  assert(entry.refs > 0);
  if (entry.refs == 1) {
    assert(entry.in_cache);
    Remove(entry);
    Append(shard.in_use, entry);
  }
  ++entry.refs;
}

void ShardedLruCacheCore::Unref(Shard& shard, Entry& entry, Entry*& retired) noexcept {
  assert(entry.refs > 0);
  --entry.refs;
  if (entry.refs == 0) {
    assert(!entry.in_cache);
    Retire(entry, retired);
  } else if (entry.in_cache && entry.refs == 1) {
    Remove(entry);
    Append(shard.lru, entry);
  }
}

void ShardedLruCacheCore::FinishErase(Shard& shard, Entry* entry, Entry*& retired) noexcept {
  if (entry == nullptr) {
    return;
  }
  assert(entry->in_cache);
  Remove(*entry);
  entry->in_cache = false;
  assert(shard.usage >= entry->charge);
  shard.usage -= entry->charge;
  Unref(shard, *entry, retired);
}

void ShardedLruCacheCore::Evict(Shard& shard, Entry*& retired) noexcept {
  while (shard.usage > shard.capacity && shard.lru.next != &shard.lru) {
    Entry* entry = shard.lru.next;
    assert(entry->refs == 1 && entry->in_cache);
    Entry* removed = shard.table.Remove(entry->key(), entry->hash);
    assert(removed == entry);
    (void)removed;
    FinishErase(shard, entry, retired);
  }
}

void ShardedLruCacheCore::Retire(Entry& entry, Entry*& retired) noexcept {
  entry.next_hash = retired;
  retired = &entry;
}

void ShardedLruCacheCore::DeleteRetired(Entry* retired) noexcept {
  while (retired != nullptr) {
    Entry* next = retired->next_hash;
    retired->next_hash = nullptr;
    DeleteEntry(retired);
    retired = next;
  }
}

void ShardedLruCacheCore::Release(Entry& entry) noexcept {
  Shard& shard = shards_[ShardIndex(entry.hash)];
  Entry* retired = nullptr;
  {
    std::lock_guard lock(shard.mutex);
    Unref(shard, entry, retired);
  }
  DeleteRetired(retired);
}

}  // namespace modern_leveldb
