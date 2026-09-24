#ifndef MODERN_LEVELDB_ENGINE_INTERNAL_ITERATOR_H_
#define MODERN_LEVELDB_ENGINE_INTERNAL_ITERATOR_H_

#include "modern_leveldb/base/bytes.h"
#include "modern_leveldb/base/result.h"

namespace modern_leveldb {

// Iterates internal keys and their values in a comparator's order. A new
// iterator is not positioned. Seek finds the first entry at or after the
// target. key() and value() require a valid position and remain valid until
// the iterator moves; Next and Prev require a valid position. A failed move
// leaves the iterator invalid, and the next seek starts over.
class InternalIterator {
 public:
  InternalIterator(const InternalIterator&) = delete;
  InternalIterator& operator=(const InternalIterator&) = delete;
  InternalIterator(InternalIterator&&) = delete;
  InternalIterator& operator=(InternalIterator&&) = delete;
  virtual ~InternalIterator() = default;

  [[nodiscard]] virtual bool valid() const noexcept = 0;
  [[nodiscard]] virtual ByteView key() const noexcept = 0;
  [[nodiscard]] virtual ByteView value() const noexcept = 0;

  [[nodiscard]] virtual Status SeekToFirst() = 0;
  [[nodiscard]] virtual Status SeekToLast() = 0;
  [[nodiscard]] virtual Status Seek(ByteView target) = 0;
  [[nodiscard]] virtual Status Next() = 0;
  [[nodiscard]] virtual Status Prev() = 0;

 protected:
  InternalIterator() = default;
};

}  // namespace modern_leveldb

#endif  // MODERN_LEVELDB_ENGINE_INTERNAL_ITERATOR_H_
