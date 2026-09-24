#ifndef MODERN_LEVELDB_ENGINE_DATABASE_H_
#define MODERN_LEVELDB_ENGINE_DATABASE_H_

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <vector>

#include "engine/db_iterator.h"
#include "engine/table_cache.h"
#include "engine/write_path.h"
#include "format/internal_key.h"
#include "format/write_batch.h"
#include "memory/memtable.h"
#include "metadata/version_set.h"
#include "modern_leveldb/base/bytes.h"
#include "modern_leveldb/base/comparator.h"
#include "modern_leveldb/base/result.h"
#include "platform/background_executor.h"
#include "platform/clock.h"
#include "platform/file_system.h"
#include "table/table.h"
#include "table/table_builder.h"
#include "wal/wal_io.h"

namespace modern_leveldb {

struct DatabaseOptions {
  // Must outlive the database.
  const Comparator* comparator = &BytewiseComparator();
  bool create_if_missing = false;
  bool error_if_exists = false;
  // The memtable switches to a new one once it uses more than this.
  std::size_t write_buffer_size = std::size_t{4} << 20U;
  // Compaction splits its outputs at this size.
  std::uint64_t max_file_size = std::uint64_t{2} << 20U;
  // Ten are kept for files other than tables.
  std::size_t max_open_files = 1000;
  TableBuilderOptions table_options{};
  // Without one, the database owns an 8 MiB block cache.
  BlockCache* block_cache = nullptr;
  // Without them, the database uses the POSIX file system, a serial executor,
  // and the system clock. Each must outlive the database. An executor must
  // queue tasks without running them in Schedule, and must eventually run
  // every task it accepts while the database is open.
  FileSystem* file_system = nullptr;
  BackgroundExecutor* executor = nullptr;
  Clock* clock = nullptr;
};

// Returns the options clipped to LevelDB's ranges: max_open_files to 74
// through 50,000, write_buffer_size to 64 KiB through 1 GiB, max_file_size to
// 1 MiB through 1 GiB, and the block size to 1 KiB through 4 MiB.
[[nodiscard]] DatabaseOptions SanitizeOptions(DatabaseOptions options);

struct DatabaseReadOptions {
  // A sequence that GetSnapshot returned and that is not yet released; without
  // one, reads see the last sequence.
  std::optional<SequenceNumber> snapshot;
  bool fill_cache = true;
};

// An open database, as LevelDB's DBImpl. Its methods are safe to call from
// several threads.
class Database final {
  struct PrivateTag {
    explicit PrivateTag() = default;
  };

 public:
  // Opens or creates the database in the directory, which it keeps locked.
  [[nodiscard]] static Result<std::unique_ptr<Database>> Open(DatabaseOptions options,
                                                              std::filesystem::path directory);

  Database(PrivateTag, const DatabaseOptions& options, std::filesystem::path directory);
  Database(const Database&) = delete;
  Database& operator=(const Database&) = delete;
  Database(Database&&) = delete;
  Database& operator=(Database&&) = delete;
  // Waits until no background task is scheduled, and then releases the
  // directory lock last. No other call may be running.
  ~Database();

  // Commits the batch, syncing the log first if asked. After a background
  // error, every write returns it.
  [[nodiscard]] Status Write(const WriteBatch& batch, bool sync);

  // Returns the key's value, or nothing if it has none or it is deleted.
  [[nodiscard]] Result<std::optional<std::vector<std::byte>>> Get(
      ByteView key, const DatabaseReadOptions& options = {});

  // Returns an iterator that must be destroyed before the database.
  [[nodiscard]] std::unique_ptr<DbIterator> NewIterator(const DatabaseReadOptions& options = {});

  // Returns the last sequence and keeps what it reads until it is released.
  // Each call needs its own release.
  [[nodiscard]] SequenceNumber GetSnapshot();
  void ReleaseSnapshot(SequenceNumber snapshot);

  // Switches to a new memtable, even an empty one, and waits until the old one
  // is flushed. Returns the background error if there is one.
  [[nodiscard]] Status FlushMemTable();

  // Waits until no background task is scheduled, and returns the background
  // error if there is one.
  [[nodiscard]] Status WaitForBackgroundWork();

 private:
  [[nodiscard]] Status Recover(const DatabaseOptions& options);
  [[nodiscard]] Status MakeRoomForWrite(std::unique_lock<std::mutex>& lock, bool force);
  [[nodiscard]] Status SwitchMemTable();
  [[nodiscard]] Status CommitWrite(std::unique_lock<std::mutex>& lock, WriteBatch& group,
                                   bool sync);
  void MaybeScheduleBackgroundWork();
  void BackgroundCall();
  void FlushImmutable(std::unique_lock<std::mutex>& lock);
  void RemoveObsoleteFiles(std::unique_lock<std::mutex>& lock);
  void RecordBackgroundError(Error error);
  [[nodiscard]] std::unexpected<Error> BackgroundError() const;

  // Owned resources, destroyed after everything that uses them.
  std::unique_ptr<FileSystem> owned_file_system_;
  std::unique_ptr<BlockCache> owned_block_cache_;
  std::unique_ptr<FileLock> lock_;

  std::size_t write_buffer_size_;
  std::uint64_t max_file_size_;
  TableBuilderOptions table_options_;
  std::filesystem::path directory_;
  FileSystem* file_system_;
  InternalKeyComparator comparator_;
  TableCache table_cache_;

  std::mutex mutex_;
  std::condition_variable background_finished_;
  std::unique_ptr<VersionSet> versions_;
  std::unique_ptr<WalWriter> log_;
  std::uint64_t log_number_ = 0;
  std::shared_ptr<MemTable> memtable_;
  std::shared_ptr<const MemTable> immutable_;
  WriteQueue write_queue_;
  std::multiset<SequenceNumber> snapshots_;
  std::set<std::uint64_t> pending_outputs_;
  std::optional<Error> background_error_;
  bool background_scheduled_ = false;
  // Mirrors state that code without the mutex checks.
  std::atomic<bool> closing_ = false;

  // Destroyed first, so that it stops before anything that its tasks use.
  std::unique_ptr<BackgroundExecutor> owned_executor_;
  BackgroundExecutor* executor_;
};

}  // namespace modern_leveldb

#endif  // MODERN_LEVELDB_ENGINE_DATABASE_H_
