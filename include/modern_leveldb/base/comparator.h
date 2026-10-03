#ifndef MODERN_LEVELDB_BASE_COMPARATOR_H_
#define MODERN_LEVELDB_BASE_COMPARATOR_H_

#include <cstddef>
#include <string_view>
#include <vector>

#include "modern_leveldb/base/bytes.h"

namespace modern_leveldb {

// Defines a deterministic total ordering of logical keys. Ordering and equality
// must remain unchanged for the lifetime of a database and across every reopen.
// All methods must support concurrent calls.
class Comparator {
 public:
  Comparator() = default;
  Comparator(const Comparator&) = delete;
  Comparator& operator=(const Comparator&) = delete;
  Comparator(Comparator&&) = delete;
  Comparator& operator=(Comparator&&) = delete;
  virtual ~Comparator() = default;

  // Returns negative, zero, or positive for less, equal, or greater logical keys.
  [[nodiscard]] virtual int Compare(ByteView left, ByteView right) const noexcept = 0;
  // Identifies exactly one ordering/equality contract in persisted metadata.
  // Reusing a name with changed semantics does not migrate existing data.
  [[nodiscard]] virtual std::string_view Name() const noexcept = 0;

  // If original start < limit, the result must remain in [original start, limit).
  // Leaving start unchanged is correct.
  virtual void FindShortestSeparator(std::vector<std::byte>& start, ByteView limit) const = 0;
  // The result must compare not less than the original key. Leaving it unchanged
  // is correct.
  virtual void FindShortSuccessor(std::vector<std::byte>& key) const = 0;
};

// Returns the shared bytewise comparator. It is destroyed during static teardown;
// callers in static destructors must ensure it was initialized before their owner.
[[nodiscard]] const Comparator& BytewiseComparator() noexcept;

}  // namespace modern_leveldb

#endif  // MODERN_LEVELDB_BASE_COMPARATOR_H_
