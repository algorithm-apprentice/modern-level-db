#ifndef MODERN_LEVELDB_API_API_INTERNAL_H_
#define MODERN_LEVELDB_API_API_INTERNAL_H_

#include <cstddef>
#include <memory>
#include <utility>

#include "engine/database.h"
#include "engine/db_iterator.h"
#include "format/write_batch.h"
#include "modern_leveldb/iterator.h"
#include "modern_leveldb/write_batch.h"

namespace modern_leveldb {

class WriteBatch::Impl final {
 public:
  Impl() = default;
  Impl(const Impl&) = default;
  Impl& operator=(const Impl&) = default;

  [[nodiscard]] EncodedWriteBatch& batch() noexcept { return batch_; }
  [[nodiscard]] const EncodedWriteBatch& batch() const noexcept { return batch_; }

 private:
  EncodedWriteBatch batch_;
};

namespace detail {

class DatabaseState final {
 public:
  DatabaseState(std::shared_ptr<const Comparator> comparator,
                std::unique_ptr<DatabaseEngine> engine) noexcept
      : comparator_(std::move(comparator)), engine_(std::move(engine)) {}

  [[nodiscard]] DatabaseEngine& engine() const noexcept { return *engine_; }

 private:
  // Reverse destruction closes the engine before releasing its comparator.
  std::shared_ptr<const Comparator> comparator_;
  std::unique_ptr<DatabaseEngine> engine_;
};

class SnapshotRegistration final {
 public:
  explicit SnapshotRegistration(std::shared_ptr<DatabaseState> state)
      : state_(std::move(state)), sequence_(state_->engine().GetSnapshot()) {}

  SnapshotRegistration(const SnapshotRegistration&) = delete;
  SnapshotRegistration& operator=(const SnapshotRegistration&) = delete;
  ~SnapshotRegistration() { state_->engine().ReleaseSnapshot(sequence_); }

  [[nodiscard]] const std::shared_ptr<DatabaseState>& state() const noexcept { return state_; }
  [[nodiscard]] SequenceNumber sequence() const noexcept { return sequence_; }

 private:
  std::shared_ptr<DatabaseState> state_;
  SequenceNumber sequence_;
};

}  // namespace detail

class Iterator::Impl final {
 public:
  Impl(std::shared_ptr<detail::DatabaseState> state,
       std::shared_ptr<detail::SnapshotRegistration> snapshot,
       std::unique_ptr<DbIterator> iterator) noexcept
      : state_(std::move(state)), snapshot_(std::move(snapshot)), iterator_(std::move(iterator)) {}

  [[nodiscard]] DbIterator& iterator() const noexcept { return *iterator_; }

 private:
  // Reverse destruction drops the private iterator, then its snapshot, then
  // the engine state that both use.
  std::shared_ptr<detail::DatabaseState> state_;
  std::shared_ptr<detail::SnapshotRegistration> snapshot_;
  std::unique_ptr<DbIterator> iterator_;
};

}  // namespace modern_leveldb

#endif  // MODERN_LEVELDB_API_API_INTERNAL_H_
