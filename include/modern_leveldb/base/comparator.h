#ifndef MODERN_LEVELDB_BASE_COMPARATOR_H_
#define MODERN_LEVELDB_BASE_COMPARATOR_H_

#include <cstddef>
#include <string_view>
#include <vector>

#include "modern_leveldb/base/bytes.h"

namespace modern_leveldb {

class Comparator {
 public:
  Comparator() = default;
  Comparator(const Comparator&) = delete;
  Comparator& operator=(const Comparator&) = delete;
  Comparator(Comparator&&) = delete;
  Comparator& operator=(Comparator&&) = delete;
  virtual ~Comparator() = default;

  [[nodiscard]] virtual int Compare(ByteView left, ByteView right) const noexcept = 0;
  [[nodiscard]] virtual std::string_view Name() const noexcept = 0;

  virtual void FindShortestSeparator(std::vector<std::byte>& start, ByteView limit) const = 0;
  virtual void FindShortSuccessor(std::vector<std::byte>& key) const = 0;
};

// Returns the shared bytewise comparator. It is destroyed during static teardown;
// callers in static destructors must ensure it was initialized before their owner.
[[nodiscard]] const Comparator& BytewiseComparator() noexcept;

}  // namespace modern_leveldb

#endif  // MODERN_LEVELDB_BASE_COMPARATOR_H_
