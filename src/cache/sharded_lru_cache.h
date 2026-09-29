#ifndef MODERN_LEVELDB_CACHE_SHARDED_LRU_CACHE_H_
#define MODERN_LEVELDB_CACHE_SHARDED_LRU_CACHE_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <utility>

#include "cache/sharded_lru_cache_core.h"
#include "modern_leveldb/base/bytes.h"
#include "modern_leveldb/base/result.h"

namespace modern_leveldb {

template <typename Value>
class ShardedLruCache final {
 public:
  class Handle final {
   public:
    Handle(const Handle&) = delete;
    Handle& operator=(const Handle&) = delete;
    Handle(Handle&&) noexcept = default;
    Handle& operator=(Handle&&) noexcept = default;
    ~Handle() = default;

    [[nodiscard]] const Value& value() const noexcept {
      return *static_cast<const Value*>(pin_.value());
    }
    [[nodiscard]] const Value* operator->() const noexcept { return &value(); }
    [[nodiscard]] const Value& operator*() const noexcept { return value(); }

   private:
    friend class ShardedLruCache;

    // GCOVR_EXCL_START: identical typed constructor repeats for each cache value type
    explicit Handle(ShardedLruCacheCore::Pin pin) noexcept : pin_(std::move(pin)) {}
    // GCOVR_EXCL_STOP

    ShardedLruCacheCore::Pin pin_;
  };

  explicit ShardedLruCache(std::size_t capacity) : core_(capacity) {}

  ShardedLruCache(const ShardedLruCache&) = delete;
  ShardedLruCache& operator=(const ShardedLruCache&) = delete;
  ShardedLruCache(ShardedLruCache&&) = delete;
  ShardedLruCache& operator=(ShardedLruCache&&) = delete;
  ~ShardedLruCache() = default;

  [[nodiscard]] Result<Handle> Insert(ByteView key, std::unique_ptr<const Value> value,
                                      std::size_t charge) {
    if (value == nullptr) {
      return std::unexpected(Error::InvalidArgument("cache value is null"));
    }
    if (charge == 0) {
      return std::unexpected(Error::InvalidArgument("cache charge must be positive"));
    }
    const Value* transferred = value.release();
    // GCOVR_EXCL_START: identical typed success mapping repeats for each cache value type
    return Handle(core_.Insert(key, transferred, &DeleteValue, charge));
    // GCOVR_EXCL_STOP
  }

  // GCOVR_EXCL_START: identical typed conversion repeats for each cache value type
  [[nodiscard]] std::optional<Handle> Lookup(ByteView key) {
    std::optional<ShardedLruCacheCore::Pin> pin = core_.Lookup(key);
    if (!pin.has_value()) {
      return std::nullopt;
    }
    return Handle(std::move(*pin));
  }
  // GCOVR_EXCL_STOP

  void Erase(ByteView key) { core_.Erase(key); }

  [[nodiscard]] std::size_t total_charge() const { return core_.total_charge(); }

  [[nodiscard]] std::uint64_t NewId() noexcept { return core_.NewId(); }

 private:
  // GCOVR_EXCL_START: identical typed deleter repeats for each cache value type
  static void DeleteValue(const void* value) noexcept { delete static_cast<const Value*>(value); }
  // GCOVR_EXCL_STOP

  ShardedLruCacheCore core_;
};

}  // namespace modern_leveldb

#endif  // MODERN_LEVELDB_CACHE_SHARDED_LRU_CACHE_H_
