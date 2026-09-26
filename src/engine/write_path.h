#ifndef MODERN_LEVELDB_ENGINE_WRITE_PATH_H_
#define MODERN_LEVELDB_ENGINE_WRITE_PATH_H_

#include <cstddef>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>

#include "format/internal_key.h"
#include "format/write_batch.h"
#include "memory/memtable.h"
#include "modern_leveldb/base/result.h"
#include "wal/wal_io.h"

namespace modern_leveldb {

// Adds the remaining entries of the batch to the memtable with their sequences
// and returns the memtable's first error.
[[nodiscard]] Status InsertBatch(WriteBatchReader& batch, MemTable& memtable);

// Sets the group's sequence after checking, without I/O, everything that could
// keep the group from reaching the memtable once it is in the log. Returns
// InvalidArgument, changing nothing, if an entry would take a sequence above
// MaxSequenceNumber or a key is longer than the memtable accepts.
[[nodiscard]] Status PrepareGroup(EncodedWriteBatch& group, SequenceNumber first_sequence);

// Appends a prepared group to the log, syncs the log if asked, and inserts the
// group into the memtable. Returns the log's first error, which the log writer
// keeps. Only one writer at a time may call it for a memtable.
[[nodiscard]] Status CommitGroup(const EncodedWriteBatch& group, bool sync, WalWriter& log,
                                 MemTable& memtable);

// Queues writers under a database mutex and commits the batches of the writers
// behind the front one with its own, as LevelDB's DBImpl::Write does.
class WriteQueue final {
 public:
  // Called by the front writer with the lock held; each may release the lock
  // while it waits or performs I/O and must hold it when it returns. Prepare
  // makes room for the write, with force set for a writer that Force queued;
  // Commit commits the group, which it may change.
  using Prepare = std::function<Status(std::unique_lock<std::mutex>& lock, bool force)>;
  using Commit = std::function<Status(std::unique_lock<std::mutex>& lock, EncodedWriteBatch& group,
                                      bool sync)>;

  WriteQueue(Prepare prepare, Commit commit);

  WriteQueue(const WriteQueue&) = delete;
  WriteQueue& operator=(const WriteQueue&) = delete;
  WriteQueue(WriteQueue&&) = delete;
  WriteQueue& operator=(WriteQueue&&) = delete;
  ~WriteQueue() = default;

  // Requires the lock on the database mutex. Waits until the batch has been
  // committed in a group or its writer is at the front. At the front, it
  // prepares, and if that succeeds, commits a group of its batch and the
  // batches of the writers queued behind it, which return the same status. A
  // failed prepare fails only this writer. If a step throws, the writers of the
  // group return Aborted and the exception propagates.
  [[nodiscard]] Status Write(std::unique_lock<std::mutex>& lock, const EncodedWriteBatch& batch,
                             bool sync);

  // Requires the lock. Queues a writer without a batch that, at the front,
  // calls prepare with force set and completes alone, as LevelDB's batch-less
  // writer does. A group ends before it.
  [[nodiscard]] Status Force(std::unique_lock<std::mutex>& lock);

  // Returns the number of queued writers, including the front one. Requires the
  // lock.
  [[nodiscard]] std::size_t size() const noexcept { return writers_.size(); }

 private:
  struct Writer;
  class LeaderGuard;

  // Queues a writer of the batch, or a forced writer without one.
  [[nodiscard]] Status Run(std::unique_lock<std::mutex>& lock, const EncodedWriteBatch* batch,
                           bool sync);
  // Copies the batches of the group that the front writer leads into group_
  // and returns the group's last writer.
  Writer* BuildGroup(const Writer& leader);
  // Removes the writers from the front through `last`, gives them the result,
  // and wakes the next writer.
  void Complete(const Writer& leader, const Writer* last,
                const std::shared_ptr<const Status>& result) noexcept;

  Prepare prepare_;
  Commit commit_;
  std::deque<Writer*> writers_;
  EncodedWriteBatch group_;
  // The result of a group whose commit threw, allocated in advance.
  std::shared_ptr<const Status> aborted_;
};

}  // namespace modern_leveldb

#endif  // MODERN_LEVELDB_ENGINE_WRITE_PATH_H_
