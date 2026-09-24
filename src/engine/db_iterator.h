#ifndef MODERN_LEVELDB_ENGINE_DB_ITERATOR_H_
#define MODERN_LEVELDB_ENGINE_DB_ITERATOR_H_

#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <vector>

#include "engine/internal_iterator.h"
#include "format/internal_key.h"
#include "modern_leveldb/base/bytes.h"
#include "modern_leveldb/base/comparator.h"
#include "modern_leveldb/base/result.h"

namespace modern_leveldb {

// Iterates the user keys whose newest entry at or before the sequence is a
// value, with that value, in the user comparator's order. key() returns the
// user key. key() and value() require a valid position and remain valid until
// the iterator moves; Next and Prev require a valid position. A failed move,
// including one that finds a key that is not an internal key, leaves the
// iterator invalid. The user comparator must outlive the iterator.
// Samples a database iterator's reads for seek statistics, as LevelDB's
// DBIter does. Either both functions are set or neither is.
struct ReadSampling {
  // Returns the number of key and value bytes to read before the next sample.
  std::function<std::uint64_t()> next_period;
  // Receives the internal key of each sampled entry.
  std::function<void(ByteView internal_key)> sample;
};

class DbIterator final {
 public:
  // Requires a sequence of at most MaxSequenceNumber. With sampling, the
  // iterator counts the key and value bytes of each entry that it examines
  // while it finds the next or previous user key, drawing the first period at
  // the first such entry, and samples the entry once for each period that the
  // count passes.
  DbIterator(std::unique_ptr<InternalIterator> internal, const Comparator& user_comparator,
             SequenceNumber sequence, ReadSampling sampling = {}) noexcept;
  DbIterator(std::unique_ptr<InternalIterator> internal, const Comparator&& user_comparator,
             SequenceNumber sequence, ReadSampling sampling = {}) = delete;

  DbIterator(const DbIterator&) = delete;
  DbIterator& operator=(const DbIterator&) = delete;
  DbIterator(DbIterator&&) = delete;
  DbIterator& operator=(DbIterator&&) = delete;
  ~DbIterator() = default;

  [[nodiscard]] bool valid() const noexcept { return valid_; }
  [[nodiscard]] ByteView key() const noexcept;
  [[nodiscard]] ByteView value() const noexcept;

  [[nodiscard]] Status SeekToFirst();
  [[nodiscard]] Status SeekToLast();
  // Finds the first user key at or after the target.
  [[nodiscard]] Status Seek(ByteView user_key);
  [[nodiscard]] Status Next();
  [[nodiscard]] Status Prev();

 private:
  [[nodiscard]] Status FindNextUserEntry(bool skipping);
  [[nodiscard]] Status FindPrevUserEntry();
  [[nodiscard]] Status Fail(Error error);
  // Counts the entry's bytes toward the next sample.
  void CountRead(ByteView key, ByteView value);

  std::unique_ptr<InternalIterator> internal_;
  const Comparator* user_comparator_;
  SequenceNumber sequence_;
  // Moving forward, the internal iterator is at the entry that the iterator
  // yields. Moving backward, the iterator yields the saved key and value, and
  // the internal iterator is at an entry before that user key's entries, with
  // only invisible entries between them, or past the beginning.
  bool forward_ = true;
  bool valid_ = false;
  std::vector<std::byte> saved_key_;
  std::vector<std::byte> saved_value_;
  ReadSampling sampling_;
  // The bytes to read before the next sample, once the first entry drew it.
  std::optional<std::uint64_t> bytes_until_sample_;
};

}  // namespace modern_leveldb

#endif  // MODERN_LEVELDB_ENGINE_DB_ITERATOR_H_
