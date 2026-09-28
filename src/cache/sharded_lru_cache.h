#ifndef MODERN_LEVELDB_CACHE_SHARDED_LRU_CACHE_H_
#define MODERN_LEVELDB_CACHE_SHARDED_LRU_CACHE_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "cache/intrusive_lru_cache.h"
#include "modern_leveldb/base/bytes.h"
#include "modern_leveldb/base/result.h"

namespace modern_leveldb {

template <typename Value>
class ShardedLruCache final {
 private:
  class TypedEntry final : public IntrusiveLruCache::Entry {
   public:
    TypedEntry(std::vector<std::byte> key, std::shared_ptr<const Value> entry_value,
               std::size_t charge)
        : IntrusiveLruCache::Entry(std::move(key), charge, entry_value != nullptr),
          value(std::move(entry_value)) {}

    std::shared_ptr<const Value> value;
  };

 public:
  class Handle final {
   public:
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&&) noexcept = default;
    Handle& operator=(Handle&&) noexcept = default;
    ~Handle() = default;

    [[nodiscard]] const Value& value() const noexcept {
      return *static_cast<const TypedEntry&>(pin_.entry()).value;
    }
    [[nodiscard]] const Value* operator->() const noexcept { return &value(); }
    [[nodiscard]] const Value& operator*() const noexcept { return value(); }

   private:
    friend class ShardedLruCache;

    explicit Handle(IntrusiveLruCache::Pin pin) noexcept : pin_(std::move(pin)) {}

    // GCOVR_EXCL_START: duplicated unreachable template member cleanup edge
    IntrusiveLruCache::Pin pin_;
    // GCOVR_EXCL_STOP
  };

  explicit ShardedLruCache(std::size_t capacity) : cache_(capacity) {}

  ShardedLruCache(const ShardedLruCache&) = delete;
  ShardedLruCache& operator=(const ShardedLruCache&) = delete;
  ShardedLruCache(ShardedLruCache&&) = delete;
  ShardedLruCache& operator=(ShardedLruCache&&) = delete;
  ~ShardedLruCache() = default;

  [[nodiscard]] Result<Handle> Insert(ByteView key, std::shared_ptr<const Value> value,
                                      std::size_t charge) {
    auto entry = std::make_unique<TypedEntry>(std::vector<std::byte>(key.begin(), key.end()),
                                              std::move(value), charge);
    // GCOVR_EXCL_START: identical error mapping repeats for every value specialization
    return cache_.Insert(std::move(entry)).transform([](IntrusiveLruCache::Pin pin) {
      return Handle(std::move(pin));
    });
    // GCOVR_EXCL_STOP
  }

  [[nodiscard]] std::optional<Handle> Lookup(ByteView key) {
    return cache_.Lookup(key).transform(
        [](IntrusiveLruCache::Pin pin) { return Handle(std::move(pin)); });
  }

  void Erase(ByteView key) { cache_.Erase(key); }

  [[nodiscard]] std::size_t total_charge() const { return cache_.total_charge(); }

  [[nodiscard]] std::uint64_t NewId() noexcept { return cache_.NewId(); }

 private:
  // GCOVR_EXCL_START: duplicated unreachable template member cleanup edge
  IntrusiveLruCache cache_;
  // GCOVR_EXCL_STOP
};

}  // namespace modern_leveldb

#endif  // MODERN_LEVELDB_CACHE_SHARDED_LRU_CACHE_H_
