#include "engine/database.h"

#include <algorithm>
#include <cassert>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stop_token>
#include <string>
#include <utility>
#include <vector>

#include "engine/flush.h"
#include "engine/iterators.h"
#include "engine/lookup.h"
#include "engine/recovery.h"
#include "metadata/filenames.h"
#include "metadata/version.h"
#include "metadata/version_edit.h"
#if defined(MODERN_LEVELDB_HAVE_POSIX_FILE_SYSTEM)
#include "platform/posix_file_system.h"
#endif

namespace modern_leveldb {
namespace {

// LevelDB's kNumNonTableCacheFiles: files other than tables that the database
// keeps open.
constexpr std::size_t NonTableFiles = 10;
// The block cache that a database owns when it is given none.
constexpr std::size_t DefaultBlockCacheSize = std::size_t{8} << 20U;

std::unique_ptr<FileSystem> OwnedFileSystem(const DatabaseOptions& options) {
  if (options.file_system != nullptr) {
    return nullptr;
  }
#if defined(MODERN_LEVELDB_HAVE_POSIX_FILE_SYSTEM)
  return std::make_unique<PosixFileSystem>();
#else
  return nullptr;
#endif
}

std::unique_ptr<BlockCache> OwnedBlockCache(const DatabaseOptions& options) {
  if (options.block_cache != nullptr) {
    return nullptr;
  }
  return std::make_unique<BlockCache>(DefaultBlockCacheSize);
}

std::unique_ptr<BackgroundExecutor> OwnedExecutor(const DatabaseOptions& options) {
  if (options.executor != nullptr) {
    return nullptr;
  }
  return std::make_unique<SerialExecutor>();
}

TableOptions ReadTableOptions(const DatabaseOptions& options, BlockCache* owned_block_cache) {
  TableOptions table_options{
      .filter_policy = options.table_options.filter_policy,
      .block_cache = options.block_cache != nullptr ? options.block_cache : owned_block_cache};
  return table_options;
}

TableReadOptions ReadOptionsFor(const DatabaseReadOptions& options) {
  TableReadOptions read_options;
  read_options.fill_cache = options.fill_cache;
  return read_options;
}

}  // namespace

DatabaseOptions SanitizeOptions(DatabaseOptions options) {
  options.max_open_files =
      std::clamp<std::size_t>(options.max_open_files, 64 + NonTableFiles, 50000);
  options.write_buffer_size = std::clamp<std::size_t>(
      options.write_buffer_size, std::size_t{64} << 10U, std::size_t{1} << 30U);
  options.max_file_size = std::clamp<std::uint64_t>(options.max_file_size, std::uint64_t{1} << 20U,
                                                    std::uint64_t{1} << 30U);
  options.table_options.block_size = std::clamp<std::size_t>(
      options.table_options.block_size, std::size_t{1} << 10U, std::size_t{4} << 20U);
  return options;
}

Result<std::unique_ptr<Database>> Database::Open(DatabaseOptions options,
                                                 std::filesystem::path directory) {
#if !defined(MODERN_LEVELDB_HAVE_POSIX_FILE_SYSTEM)
  // Only the POSIX backend exists (ADR-0011), so elsewhere the caller gives
  // the file system.
  if (options.file_system == nullptr) {
    return std::unexpected(Error::NotSupported("this platform has no default file system"));
  }
#endif
  options = SanitizeOptions(std::move(options));
  auto database = std::make_unique<Database>(PrivateTag(), options, std::move(directory));
  const Status recovered = database->Recover(options);
  if (!recovered.has_value()) {
    return std::unexpected(recovered.error());
  }
  return database;
}

Database::Database(PrivateTag, const DatabaseOptions& options, std::filesystem::path directory)
    : owned_file_system_(OwnedFileSystem(options)),
      owned_block_cache_(OwnedBlockCache(options)),
      write_buffer_size_(options.write_buffer_size),
      max_file_size_(options.max_file_size),
      table_options_(options.table_options),
      directory_(std::move(directory)),
      file_system_(options.file_system != nullptr ? options.file_system : owned_file_system_.get()),
      comparator_(*options.comparator),
      table_cache_(*file_system_, directory_, comparator_,
                   ReadTableOptions(options, owned_block_cache_.get()),
                   options.max_open_files - NonTableFiles),
      write_queue_([this](std::unique_lock<std::mutex>& lock,
                          bool force) { return MakeRoomForWrite(lock, force); },
                   [this](std::unique_lock<std::mutex>& lock, WriteBatch& group, bool sync) {
                     return CommitWrite(lock, group, sync);
                   }),
      owned_executor_(OwnedExecutor(options)),
      executor_(options.executor != nullptr ? options.executor : owned_executor_.get()) {}

Database::~Database() {
  std::unique_lock lock(mutex_);
  closing_ = true;
  background_finished_.wait(lock, [this] { return !background_scheduled_; });
}

Status Database::Recover(const DatabaseOptions& options) {
  RecoveryOptions recovery{.create_if_missing = options.create_if_missing,
                           .error_if_exists = options.error_if_exists,
                           .write_buffer_size = options.write_buffer_size,
                           .table_options = options.table_options};
  Result<RecoveredDatabase> recovered =
      RecoverDatabase(*file_system_, directory_, comparator_, recovery, table_cache_);
  if (!recovered.has_value()) {
    return std::unexpected(std::move(recovered).error());
  }
  std::unique_lock lock(mutex_);
  lock_ = std::move(recovered->lock);
  versions_ = std::move(recovered->versions);
  log_ = std::move(recovered->log);
  log_number_ = recovered->log_number;
  memtable_ = std::make_shared<MemTable>(comparator_.user_comparator());
  RemoveObsoleteFiles(lock);
  return {};
}

Status Database::Write(const WriteBatch& batch, bool sync) {
  std::unique_lock lock(mutex_);
  return write_queue_.Write(lock, batch, sync);
}

Status Database::MakeRoomForWrite(std::unique_lock<std::mutex>& lock, bool force) {
  while (true) {
    if (background_error_.has_value()) {
      return BackgroundError();
    }
    if (!force && memtable_->memory_usage() <= write_buffer_size_) {
      return {};
    }
    if (immutable_ != nullptr) {
      background_finished_.wait(lock);
      continue;
    }
    const Status switched = SwitchMemTable();
    if (!switched.has_value()) {
      return switched;
    }
    force = false;
  }
}

Status Database::SwitchMemTable() {
  const std::uint64_t number = versions_->NewFileNumber();
  const std::filesystem::path path = LogFileName(directory_, number);
  Result<std::unique_ptr<WritableFile>> file = file_system_->OpenWritable(path);
  if (!file.has_value()) {
    return std::unexpected(std::move(file).error());
  }
  // The allocations come before any state changes.
  auto new_log = std::make_unique<WalWriter>(std::move(*file));
  auto new_memtable = std::make_shared<MemTable>(comparator_.user_comparator());
  // The flush of the old memtable makes this the oldest log that recovery
  // replays, so its directory entry is durable before it takes writes.
  const Status synced = file_system_->SyncDirectory(directory_);
  if (!synced.has_value()) {
    return synced;
  }
  const Status closed = log_->Close();
  if (!closed.has_value()) {
    RecordBackgroundError(closed.error());
  }
  log_ = std::move(new_log);
  log_number_ = number;
  immutable_ = std::move(memtable_);
  memtable_ = std::move(new_memtable);
  MaybeScheduleBackgroundWork();
  return {};
}

Status Database::CommitWrite(std::unique_lock<std::mutex>& lock, WriteBatch& group, bool sync) {
  const SequenceNumber first = versions_->last_sequence() + 1;
  const Status prepared = PrepareGroup(group, first);
  if (!prepared.has_value()) {
    return prepared;
  }
  const std::uint32_t count = group.count();
  // Only the front writer switches the log and the memtable, so they stay.
  WalWriter& log = *log_;
  MemTable& memtable = *memtable_;
  lock.unlock();
  Status committed;
  try {
    committed = CommitGroup(group, sync, log, memtable);
  } catch (...) {
    lock.lock();
    RecordBackgroundError(Error::Aborted("a write stopped with an exception while it committed"));
    throw;
  }
  lock.lock();
  if (!committed.has_value()) {
    RecordBackgroundError(committed.error());
    return committed;
  }
  // An empty group leaves the last sequence as it was.
  versions_->SetLastSequence(first + count - 1);
  return {};
}

Result<std::optional<std::vector<std::byte>>> Database::Get(ByteView key,
                                                            const DatabaseReadOptions& options) {
  std::unique_lock lock(mutex_);
  const SequenceNumber sequence =
      options.snapshot.has_value() ? *options.snapshot : versions_->last_sequence();
  const std::shared_ptr<const MemTable> memtable = memtable_;
  const std::shared_ptr<const MemTable> immutable = immutable_;
  const std::shared_ptr<const Version> version = versions_->current();
  lock.unlock();

  Result<LookupKey> lookup_key = LookupKey::Create(key, sequence);
  if (!lookup_key.has_value()) {  // GCOVR_EXCL_BR_WITHOUT_HIT: 1/2 needs a key over 4 GiB
    return std::unexpected(std::move(lookup_key).error());  // GCOVR_EXCL_LINE: needs over 4 GiB
  }
  Result<PointRead> read = LookupValue(*memtable, immutable.get(), *version, table_cache_,
                                       comparator_, *lookup_key, ReadOptionsFor(options));
  if (!read.has_value()) {
    return std::unexpected(std::move(read).error());
  }
  return std::move(read->value);
}

std::unique_ptr<DbIterator> Database::NewIterator(const DatabaseReadOptions& options) {
  std::unique_lock lock(mutex_);
  const SequenceNumber sequence =
      options.snapshot.has_value() ? *options.snapshot : versions_->last_sequence();
  std::unique_ptr<InternalIterator> internal =
      NewInternalIterator(memtable_, immutable_, versions_->current(), table_cache_, comparator_,
                          ReadOptionsFor(options));
  lock.unlock();
  return std::make_unique<DbIterator>(std::move(internal), comparator_.user_comparator(), sequence);
}

SequenceNumber Database::GetSnapshot() {
  const std::lock_guard lock(mutex_);
  const SequenceNumber sequence = versions_->last_sequence();
  snapshots_.insert(sequence);
  return sequence;
}

void Database::ReleaseSnapshot(SequenceNumber snapshot) {
  const std::lock_guard lock(mutex_);
  const auto found = snapshots_.find(snapshot);
  assert(found != snapshots_.end());
  snapshots_.erase(found);
}

Status Database::FlushMemTable() {
  std::unique_lock lock(mutex_);
  const Status forced = write_queue_.Force(lock);
  if (!forced.has_value()) {
    return forced;
  }
  // The forced switch made the immutable memtable, and the lock stayed held.
  const std::shared_ptr<const MemTable> flushing = immutable_;
  background_finished_.wait(
      lock, [&] { return immutable_ != flushing || background_error_.has_value(); });
  if (background_error_.has_value()) {
    return BackgroundError();
  }
  return {};
}

Status Database::WaitForBackgroundWork() {
  std::unique_lock lock(mutex_);
  background_finished_.wait(lock, [this] { return !background_scheduled_; });
  if (background_error_.has_value()) {
    return BackgroundError();
  }
  return {};
}

void Database::MaybeScheduleBackgroundWork() {
  if (background_scheduled_ || closing_ || background_error_.has_value() || immutable_ == nullptr) {
    return;
  }
  background_scheduled_ = true;
  Status scheduled;
  try {
    scheduled = executor_->Schedule([this](std::stop_token) { BackgroundCall(); });
  } catch (...) {
    scheduled = std::unexpected(Error::Aborted("scheduling background work threw an exception"));
  }
  if (!scheduled.has_value()) {
    background_scheduled_ = false;
    RecordBackgroundError(scheduled.error());
  }
}

void Database::BackgroundCall() {
  std::unique_lock lock(mutex_);
  if (!closing_ && !background_error_.has_value()) {
    try {
      FlushImmutable(lock);
    } catch (...) {
      if (!lock.owns_lock()) {
        lock.lock();
      }
      RecordBackgroundError(Error::Aborted("background work stopped with an exception"));
    }
  }
  background_scheduled_ = false;
  MaybeScheduleBackgroundWork();
  background_finished_.notify_all();
}

void Database::FlushImmutable(std::unique_lock<std::mutex>& lock) {
  const std::shared_ptr<const Version> base = versions_->current();
  const std::uint64_t number = versions_->NewFileNumber();
  pending_outputs_.insert(number);
  const std::shared_ptr<const MemTable> immutable = immutable_;
  const std::uint64_t log_number = log_number_;
  lock.unlock();
  FlushOptions options;
  options.table_options = table_options_;
  options.target_file_size = max_file_size_;
  Result<VersionEdit> edit =
      modern_leveldb::FlushMemTable(*file_system_, directory_, comparator_, options, table_cache_,
                                    *immutable, number, *base, log_number);
  lock.lock();
  Status applied;
  if (edit.has_value()) {
    applied = versions_->LogAndApply(std::move(*edit));
  } else {
    applied = std::unexpected(std::move(edit).error());
  }
  if (!applied.has_value()) {
    // Recorded first, so that no cleanup removes a table that the edit may
    // still reference.
    RecordBackgroundError(applied.error());
    pending_outputs_.erase(number);
    return;
  }
  pending_outputs_.erase(number);
  immutable_.reset();
  background_finished_.notify_all();
  RemoveObsoleteFiles(lock);
}

void Database::RemoveObsoleteFiles(std::unique_lock<std::mutex>& lock) {
  if (background_error_.has_value()) {
    return;
  }
  // Recovery and every flush set the previous log number to zero.
  assert(versions_->prev_log_number() == 0);
  std::set<std::uint64_t> live = versions_->LiveFiles();
  live.insert(pending_outputs_.begin(), pending_outputs_.end());
  const Result<std::vector<std::filesystem::path>> children =
      file_system_->ListDirectory(directory_);
  if (!children.has_value()) {
    return;
  }
  std::vector<std::filesystem::path> obsolete;
  for (const std::filesystem::path& child : *children) {
    const std::optional<ParsedFileName> parsed = ParseFileName(child.filename().string());
    if (!parsed.has_value()) {
      continue;
    }
    bool keep = true;
    switch (parsed->type) {  // GCOVR_EXCL_BR_WITHOUT_HIT: 1/5 default of an exhaustive switch
      case FileType::Log:
        keep = parsed->number >= versions_->log_number();
        break;
      case FileType::Descriptor:
        keep = parsed->number >= versions_->manifest_file_number();
        break;
      case FileType::Table:
      case FileType::Temp:
        keep = live.contains(parsed->number);
        break;
      case FileType::Lock:
      case FileType::Current:
        break;
    }
    if (keep) {
      continue;
    }
    if (parsed->type == FileType::Table) {
      table_cache_.Evict(parsed->number);
    }
    obsolete.push_back(directory_ / child.filename());
  }
  lock.unlock();
  for (const std::filesystem::path& path : obsolete) {
    static_cast<void>(file_system_->RemoveFile(path));
  }
  lock.lock();
}

void Database::RecordBackgroundError(Error error) {
  // Keeps the first error.
  background_error_ = background_error_.value_or(std::move(error));
  background_finished_.notify_all();
}

std::unexpected<Error> Database::BackgroundError() const {
  return std::unexpected(*background_error_);
}

}  // namespace modern_leveldb
