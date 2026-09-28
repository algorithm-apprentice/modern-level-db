#ifndef MODERN_LEVELDB_CACHE_INTRUSIVE_LRU_CACHE_H_
#define MODERN_LEVELDB_CACHE_INTRUSIVE_LRU_CACHE_H_

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "modern_leveldb/base/bytes.h"
#include "modern_leveldb/base/result.h"

namespace modern_leveldb {

class IntrusiveLruCache final {
 private:
  class Implementation;

 public:
  class Entry {
   public:
    Entry(std::vector<std::byte> key, std::size_t charge, bool has_value);
    Entry(const Entry&) = delete;
    Entry& operator=(const Entry&) = delete;
    virtual ~Entry();

   private:
    friend class Implementation;

    std::vector<std::byte> key_;
    std::size_t charge_;
    std::uint32_t hash_;
    std::size_t refs_ = 1;
    bool has_value_;
    bool in_cache_ = false;
    Entry* next_hash_ = nullptr;
    Entry* next_ = nullptr;
    Entry* previous_ = nullptr;
  };

  class Pin final {
   public:
    Pin(const Pin&) = delete;
    Pin& operator=(const Pin&) = delete;
    Pin(Pin&& source) noexcept;
    Pin& operator=(Pin&& source) noexcept;
    ~Pin();

    [[nodiscard]] const Entry& entry() const noexcept;

   private:
    friend class IntrusiveLruCache;
    friend class Implementation;

    Pin(Implementation& implementation, Entry& entry) noexcept;
    void Reset() noexcept;

    Implementation* implementation_ = nullptr;
    Entry* entry_ = nullptr;
  };

  explicit IntrusiveLruCache(std::size_t capacity);
  IntrusiveLruCache(const IntrusiveLruCache&) = delete;
  IntrusiveLruCache& operator=(const IntrusiveLruCache&) = delete;
  IntrusiveLruCache(IntrusiveLruCache&&) = delete;
  IntrusiveLruCache& operator=(IntrusiveLruCache&&) = delete;
  ~IntrusiveLruCache();

  [[nodiscard]] Result<Pin> Insert(std::unique_ptr<Entry> entry);
  [[nodiscard]] std::optional<Pin> Lookup(ByteView key);
  void Erase(ByteView key);
  [[nodiscard]] std::size_t total_charge() const;
  [[nodiscard]] std::uint64_t NewId() noexcept;

 private:
  std::unique_ptr<Implementation> implementation_;
};

}  // namespace modern_leveldb

#endif  // MODERN_LEVELDB_CACHE_INTRUSIVE_LRU_CACHE_H_
