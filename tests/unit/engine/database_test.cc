#include "engine/database.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "engine/build_table.h"
#include "engine/compaction_picker.h"
#include "engine/table_cache.h"
#include "format/internal_key.h"
#include "format/write_batch.h"
#include "memory/memtable.h"
#include "metadata/filenames.h"
#include "metadata/version.h"
#include "metadata/version_edit.h"
#include "metadata/version_set.h"
#include "modern_leveldb/base/bytes.h"
#include "modern_leveldb/base/comparator.h"
#include "modern_leveldb/base/result.h"
#include "platform/clock.h"
#include "support/manual_clock.h"
#include "support/manual_executor.h"
#include "support/memory_file_system.h"
#include "table/table.h"
#include "table/table_builder.h"

namespace modern_leveldb {
namespace {

using test_support::ManualClock;
using test_support::ManualExecutor;
using test_support::MemoryFileSystem;

std::string Text(ByteView bytes) { return std::string(AsStringView(bytes)); }

std::string Numbered(std::uint64_t number, std::string_view suffix) {
  std::string name = std::to_string(number);
  return std::string(6 - std::min<std::size_t>(6, name.size()), '0') + name + std::string(suffix);
}

// A file system hook that blocks the first operation that a predicate matches
// until the test opens the gate.
class Gate {
 public:
  void Close(std::function<bool(std::string_view)> matches) {
    const std::lock_guard lock(mutex_);
    matches_ = std::move(matches);
    open_ = false;
    reached_ = false;
  }

  void Open() {
    const std::lock_guard lock(mutex_);
    open_ = true;
    changed_.notify_all();
  }

  void WaitUntilReached() {
    std::unique_lock lock(mutex_);
    changed_.wait(lock, [this] { return reached_; });
  }

  Status operator()(std::string_view operation) {
    std::unique_lock lock(mutex_);
    if (open_ || reached_ || !matches_(operation)) {
      return {};
    }
    reached_ = true;
    changed_.notify_all();
    changed_.wait(lock, [this] { return open_; });
    return {};
  }

 private:
  std::mutex mutex_;
  std::condition_variable changed_;
  std::function<bool(std::string_view)> matches_;
  bool open_ = true;
  bool reached_ = false;
};

bool OpensATable(std::string_view operation) {
  return operation.starts_with("open_writable ") && operation.ends_with(".ldb");
}

class DatabaseTest : public testing::Test {
 protected:
  DatabaseTest() {
    options_.file_system = &file_system_;
    options_.executor = &executor_;
    options_.clock = &clock_;
    options_.create_if_missing = true;
    // Clipped to the smallest write buffer, 64 KiB.
    options_.write_buffer_size = 1;
  }

  Result<std::unique_ptr<Database>> TryOpen() { return Database::Open(options_, directory_); }

  std::unique_ptr<Database> Open() {
    Result<std::unique_ptr<Database>> database = TryOpen();
    EXPECT_TRUE(database.has_value()) << database.error().ToString();
    return database.has_value() ? std::move(*database) : nullptr;
  }

  static Status Put(Database& database, std::string_view key, std::string_view value,
                    bool sync = false) {
    WriteBatch batch;
    EXPECT_TRUE(batch.Put(AsBytes(key), AsBytes(value)).has_value());
    return database.Write(batch, sync);
  }

  static Status Delete(Database& database, std::string_view key) {
    WriteBatch batch;
    EXPECT_TRUE(batch.Delete(AsBytes(key)).has_value());
    return database.Write(batch, false);
  }

  // The value, "<none>" for a missing key, or "<error>".
  static std::string Get(Database& database, std::string_view key,
                         std::optional<SequenceNumber> snapshot = std::nullopt) {
    DatabaseReadOptions options;
    options.snapshot = snapshot;
    const auto value = database.Get(AsBytes(key), options);
    if (!value.has_value()) {
      return "<error>";
    }
    return value->has_value() ? Text(**value) : "<none>";
  }

  static std::vector<std::string> Scan(Database& database,
                                       std::optional<SequenceNumber> snapshot = std::nullopt) {
    DatabaseReadOptions options;
    options.snapshot = snapshot;
    const std::unique_ptr<DbIterator> iterator = database.NewIterator(options);
    std::vector<std::string> entries;
    Status moved = iterator->SeekToFirst();
    while (moved.has_value() && iterator->valid()) {
      entries.push_back(Text(iterator->key()) + "=" + Text(iterator->value()));
      moved = iterator->Next();
    }
    EXPECT_TRUE(moved.has_value());
    return entries;
  }

  // A value larger than the smallest write buffer, 64 KiB.
  static std::string Large() { return std::string(66000, 'v'); }

  // Writes a large value into a memtable with room, so that the memtable no
  // longer has room and the next write switches it.
  static void Fill(Database& database, std::string_view key) {
    ASSERT_TRUE(Put(database, key, Large()).has_value());
  }

  // The numbers of the directory's files of the type.
  std::vector<std::uint64_t> Numbers(FileType type) const {
    const Result<std::vector<std::filesystem::path>> names = file_system_.ListDirectory(directory_);
    std::vector<std::uint64_t> numbers;
    for (const std::filesystem::path& name : names.value()) {
      const std::optional<ParsedFileName> parsed = ParseFileName(name.string());
      if (parsed.has_value() && parsed->type == type) {
        numbers.push_back(parsed->number);
      }
    }
    std::ranges::sort(numbers);
    return numbers;
  }

  bool Exists(std::string_view name) const {
    return file_system_.Contents(directory_ / std::string(name)).has_value();
  }

  std::vector<std::string> OperationsSince(std::size_t start) const {
    return std::vector<std::string>(
        file_system_.operations().begin() + static_cast<std::ptrdiff_t>(start),
        file_system_.operations().end());
  }

  // Flushes the memtable, running its background task on this thread.
  Status Flush(Database& database) {
    std::optional<Status> status;
    std::thread flusher([&] { status = database.FlushMemTable(); });
    executor_.WaitForTask();
    executor_.RunAll();
    flusher.join();
    return *status;
  }

  // Makes the next file operation with this offset from now fail.
  void FailInOperations(std::size_t offset) {
    file_system_.FailOperation(file_system_.operations().size() + offset,
                               Error::Io("injected failure"));
  }

  // The numbers of the files at each level of the version in the MANIFEST.
  std::vector<std::vector<std::uint64_t>> Files() {
    const InternalKeyComparator comparator(BytewiseComparator());
    auto versions = VersionSet::Recover(file_system_, directory_, comparator);
    std::vector<std::vector<std::uint64_t>> files(NumLevels);
    EXPECT_TRUE(versions.has_value());
    if (versions.has_value()) {
      for (std::uint32_t level = 0; level < NumLevels; ++level) {
        for (const Version::File& file : (*versions)->current()->files(level)) {
          files[level].push_back(file->number);
        }
      }
    }
    return files;
  }

  // The number of files at each level, such as "4 1 1 0 0 0 0".
  std::string Levels() {
    std::string levels;
    for (const std::vector<std::uint64_t>& numbers : Files()) {
      levels += (levels.empty() ? "" : " ") + std::to_string(numbers.size());
    }
    return levels;
  }

  // Adds to the closed database's version a table of the keys at each level,
  // where each key's value is the key, at sequences after the last.
  void AddTables(const std::vector<std::pair<std::uint32_t, std::vector<std::string>>>& tables) {
    const InternalKeyComparator comparator(BytewiseComparator());
    auto versions = VersionSet::Recover(file_system_, directory_, comparator);
    ASSERT_TRUE(versions.has_value());
    TableCache cache(file_system_, directory_, comparator, TableOptions(), 16);
    VersionEdit edit;
    SequenceNumber sequence = (*versions)->last_sequence();
    for (const auto& [level, keys] : tables) {
      MemTable memtable(BytewiseComparator());
      for (const std::string& key : keys) {
        ASSERT_TRUE(
            memtable.Add(++sequence, ValueKind::Value, AsBytes(key), AsBytes(key)).has_value());
      }
      auto built = BuildTable(file_system_, directory_, comparator, TableBuilderOptions(), cache,
                              memtable, (*versions)->NewFileNumber());
      ASSERT_TRUE(built.has_value() && built->has_value());
      ASSERT_TRUE(edit.AddFile(level, std::move(built).value().value()).has_value());
    }
    ASSERT_TRUE(file_system_.SyncDirectory(directory_).has_value());
    (*versions)->SetLastSequence(sequence);
    ASSERT_TRUE((*versions)->LogAndApply(std::move(edit)).has_value());
  }

  // Switches a memtable that holds "z" and "a" with an empty write, which
  // leaves the new memtable and its log empty, and runs one queued task,
  // which flushes the old memtable.
  void Cycle(Database& database) {
    ASSERT_TRUE(Put(database, "z", "1").has_value());
    Fill(database, "a");
    ASSERT_TRUE(database.Write(WriteBatch(), false).has_value());
    ASSERT_TRUE(executor_.RunOne());
  }

  // A flush lands in level 0 only if it overlaps level 0 or level 1, so two
  // flushes fill levels 2 and 1 first, and each later cycle adds a level-0
  // file.
  void WarmUp(Database& database) {
    Cycle(database);
    Cycle(database);
    ASSERT_EQ(Levels(), "0 1 1 0 0 0 0");
  }

  // Adds level-0 files to a warmed-up database until a compaction of them is
  // queued, and runs it.
  void CompactLevel0(Database& database) {
    for (std::uint32_t i = 0; i < Level0CompactionTrigger; ++i) {
      Cycle(database);
    }
    ASSERT_EQ(executor_.queued(), 1U);
    ASSERT_TRUE(executor_.RunOne());
    ASSERT_EQ(Levels(), "0 1 1 0 0 0 0");
  }

  // Matches the close of the current log. The destructor closes it right
  // after it marks the database as closing, and a switch closes it after it
  // opens the next log.
  std::function<bool(std::string_view)> ClosesTheLog() const {
    const std::string close = "close " + Numbered(Numbers(FileType::Log).back(), ".log");
    return [close](std::string_view operation) { return operation == close; };
  }

  // Destroys the database with tasks queued: once the destructor marks the
  // database as closing, the tasks run and do nothing. Returns how many ran.
  int CloseWithQueuedTasks(std::unique_ptr<Database>& database) {
    Gate closing;
    closing.Close(ClosesTheLog());
    file_system_.SetOperationHook(std::ref(closing));
    std::thread closer([&] { database.reset(); });
    closing.WaitUntilReached();
    closing.Open();
    const int ran = executor_.RunAll();
    closer.join();
    file_system_.SetOperationHook({});
    return ran;
  }

  // The value that an iterator at the snapshot finds for the key, or "<none>".
  static std::string Seek(Database& database, std::string_view key,
                          std::optional<SequenceNumber> snapshot = std::nullopt) {
    DatabaseReadOptions options;
    options.snapshot = snapshot;
    const std::unique_ptr<DbIterator> iterator = database.NewIterator(options);
    EXPECT_TRUE(iterator->Seek(AsBytes(key)).has_value());
    if (!iterator->valid() || Text(iterator->key()) != key) {
      return "<none>";
    }
    return Text(iterator->value());
  }

  // Reads every entry, forward, in each of the passes.
  static void ScanRepeatedly(Database& database, int passes) {
    const std::unique_ptr<DbIterator> iterator = database.NewIterator();
    for (int pass = 0; pass < passes; ++pass) {
      Status moved = iterator->SeekToFirst();
      while (moved.has_value() && iterator->valid()) {
        moved = iterator->Next();
      }
      ASSERT_TRUE(moved.has_value());
    }
  }

  // The number of the clock's sleeps, each of which must be a millisecond.
  std::size_t Sleeps() const {
    const std::vector<Clock::Duration> sleeps = clock_.sleeps();
    for (const Clock::Duration& sleep : sleeps) {
      EXPECT_TRUE(sleep == std::chrono::milliseconds(1));
    }
    return sleeps.size();
  }

  MemoryFileSystem file_system_;
  ManualExecutor executor_;
  ManualClock clock_;
  DatabaseOptions options_;
  const std::filesystem::path directory_ = std::filesystem::path("db");
};

TEST(DatabaseOptionsTest, ClipsOptionsToLevelDbsRanges) {
  DatabaseOptions small;
  small.max_open_files = 1;
  small.write_buffer_size = 1;
  small.max_file_size = 1;
  small.table_options.block_size = 1;
  const DatabaseOptions low = SanitizeOptions(small);
  EXPECT_EQ(low.max_open_files, 74U);
  EXPECT_EQ(low.write_buffer_size, std::size_t{64} << 10U);
  EXPECT_EQ(low.max_file_size, std::uint64_t{1} << 20U);
  EXPECT_EQ(low.table_options.block_size, std::size_t{1} << 10U);

  DatabaseOptions large;
  large.max_open_files = 1000000;
  large.write_buffer_size = std::size_t{1} << 40U;
  large.max_file_size = std::uint64_t{1} << 40U;
  large.table_options.block_size = std::size_t{1} << 30U;
  const DatabaseOptions high = SanitizeOptions(large);
  EXPECT_EQ(high.max_open_files, 50000U);
  EXPECT_EQ(high.write_buffer_size, std::size_t{1} << 30U);
  EXPECT_EQ(high.max_file_size, std::uint64_t{1} << 30U);
  EXPECT_EQ(high.table_options.block_size, std::size_t{4} << 20U);

  const DatabaseOptions defaults = SanitizeOptions(DatabaseOptions());
  EXPECT_EQ(defaults.max_open_files, 1000U);
  EXPECT_EQ(defaults.write_buffer_size, std::size_t{4} << 20U);
  EXPECT_EQ(defaults.max_file_size, std::uint64_t{2} << 20U);
  EXPECT_EQ(defaults.table_options.block_size, std::size_t{4} << 10U);
}

TEST_F(DatabaseTest, OpensNewAndExistingDatabases) {
  options_.create_if_missing = false;
  const auto missing = TryOpen();
  ASSERT_FALSE(missing.has_value());
  EXPECT_EQ(missing.error().code(), ErrorCode::InvalidArgument);

  options_.create_if_missing = true;
  {
    const auto database = Open();
    ASSERT_TRUE(Put(*database, "a", "1").has_value());
    // A second database cannot lock the directory.
    const auto locked = TryOpen();
    ASSERT_FALSE(locked.has_value());
    EXPECT_EQ(locked.error().code(), ErrorCode::Busy);
  }

  // Writes that only the log holds are recovered.
  options_.create_if_missing = false;
  {
    const auto database = Open();
    EXPECT_EQ(Get(*database, "a"), "1");
  }
  options_.error_if_exists = true;
  const auto exists = TryOpen();
  ASSERT_FALSE(exists.has_value());
  EXPECT_EQ(exists.error().code(), ErrorCode::InvalidArgument);
}

TEST_F(DatabaseTest, WritesAndReadsValuesAndDeletions) {
  const auto database = Open();
  ASSERT_TRUE(Put(*database, "a", "1").has_value());
  ASSERT_TRUE(Put(*database, "b", "2").has_value());
  ASSERT_TRUE(Delete(*database, "a").has_value());
  ASSERT_TRUE(Put(*database, "b", "3").has_value());

  EXPECT_EQ(Get(*database, "a"), "<none>");
  EXPECT_EQ(Get(*database, "b"), "3");
  EXPECT_EQ(Get(*database, "c"), "<none>");
  EXPECT_EQ(Scan(*database), (std::vector<std::string>{"b=3"}));

  // An empty batch takes no sequence.
  const SequenceNumber before = database->GetSnapshot();
  ASSERT_TRUE(database->Write(WriteBatch(), false).has_value());
  const SequenceNumber after = database->GetSnapshot();
  EXPECT_EQ(after, before);
  EXPECT_EQ(before, 4U);
  database->ReleaseSnapshot(before);
  database->ReleaseSnapshot(after);
}

TEST_F(DatabaseTest, SyncsTheLogOnlyForSyncWrites) {
  const auto database = Open();
  const std::string log = Numbered(Numbers(FileType::Log).back(), ".log");

  std::size_t start = file_system_.operations().size();
  ASSERT_TRUE(Put(*database, "a", "1").has_value());
  EXPECT_EQ(std::ranges::count(OperationsSince(start), "sync " + log), 0);
  start = file_system_.operations().size();
  ASSERT_TRUE(Put(*database, "b", "2", true).has_value());
  EXPECT_EQ(std::ranges::count(OperationsSince(start), "sync " + log), 1);
}

TEST_F(DatabaseTest, CommitsConcurrentWrites) {
  const auto database = Open();
  std::vector<std::thread> writers;
  for (int writer = 0; writer < 4; ++writer) {
    writers.emplace_back([&, writer] {
      for (int write = 0; write < 50; ++write) {
        const std::string key = std::to_string(writer) + ":" + std::to_string(write);
        EXPECT_TRUE(Put(*database, key, key).has_value());
      }
    });
  }
  for (std::thread& writer : writers) {
    writer.join();
  }

  for (int writer = 0; writer < 4; ++writer) {
    for (int write = 0; write < 50; ++write) {
      const std::string key = std::to_string(writer) + ":" + std::to_string(write);
      EXPECT_EQ(Get(*database, key), key);
    }
  }
  EXPECT_EQ(Scan(*database).size(), 200U);
}

TEST_F(DatabaseTest, SwitchesToASyncedNewLogWhenTheMemtableFills) {
  const auto database = Open();
  const std::uint64_t old_log = Numbers(FileType::Log).back();
  Fill(*database, "a");
  EXPECT_EQ(executor_.queued(), 0U);

  // The next write switches to a new log, whose directory entry is synced
  // before it takes the write.
  const std::size_t start = file_system_.operations().size();
  ASSERT_TRUE(Put(*database, "b", "1").has_value());
  const std::vector<std::string> operations = OperationsSince(start);
  ASSERT_GE(operations.size(), 4U);
  const std::string new_log = operations[0].substr(std::string("open_writable ").size());
  EXPECT_EQ(operations[0], "open_writable " + new_log);
  EXPECT_EQ(operations[1], "sync_directory db");
  EXPECT_EQ(operations[2], "close " + Numbered(old_log, ".log"));
  EXPECT_EQ(operations[3], "append " + new_log);
  EXPECT_EQ(executor_.queued(), 1U);

  // The immutable memtable is readable until its flush.
  EXPECT_EQ(Get(*database, "a"), Large());
  EXPECT_EQ(Scan(*database).size(), 2U);
  EXPECT_EQ(executor_.RunAll(), 1);
  EXPECT_EQ(Numbers(FileType::Table).size(), 1U);
  EXPECT_EQ(Get(*database, "a"), Large());
  EXPECT_EQ(Get(*database, "b"), "1");
}

TEST_F(DatabaseTest, SchedulesTheNextFlushWhenTheMemtableSwitchesDuringCleanup) {
  const auto database = Open();
  const std::uint64_t first_log = Numbers(FileType::Log).back();
  Fill(*database, "a");
  ASSERT_TRUE(Put(*database, "b", "1").has_value());
  Fill(*database, "c");
  // Cleanup after the first flush removes the first log with the mutex
  // released; hold it there.
  Gate gate;
  const std::string removal = "remove " + Numbered(first_log, ".log");
  gate.Close([&](std::string_view operation) { return operation == removal; });
  file_system_.SetOperationHook(std::ref(gate));
  std::optional<int> ran;
  std::thread runner([&] { ran = executor_.RunAll(); });
  gate.WaitUntilReached();

  // The memtable switches while the first task is still scheduled, so that
  // task schedules the next flush when it ends.
  ASSERT_TRUE(Put(*database, "d", "1").has_value());
  EXPECT_EQ(executor_.queued(), 0U);
  gate.Open();
  runner.join();
  file_system_.SetOperationHook({});

  EXPECT_EQ(ran, 2);
  EXPECT_EQ(Numbers(FileType::Table).size(), 2U);
  EXPECT_EQ(Get(*database, "c"), Large());
  EXPECT_EQ(Get(*database, "d"), "1");
}

TEST_F(DatabaseTest, WritersWaitOnlyWhileAnImmutableMemtableExists) {
  const auto database = Open();
  const std::uint64_t first_log = Numbers(FileType::Log).back();
  Fill(*database, "a");
  ASSERT_TRUE(Put(*database, "b", "1").has_value());
  Fill(*database, "c");
  // Hold the flush while it writes its table, and then while its cleanup
  // removes the first log.
  Gate table;
  table.Close(OpensATable);
  Gate cleanup;
  const std::string removal = "remove " + Numbered(first_log, ".log");
  cleanup.Close([&](std::string_view operation) { return operation == removal; });
  file_system_.SetOperationHook([&](std::string_view operation) {
    static_cast<void>(table(operation));
    return cleanup(operation);
  });
  std::optional<int> ran;
  std::thread runner([&] { ran = executor_.RunAll(); });
  table.WaitUntilReached();

  // This write needs a new memtable, so it waits for the flush. The pause lets
  // it start waiting before the flush continues.
  std::promise<Status> promise;
  std::future<Status> written = promise.get_future();
  std::thread writer([&] { promise.set_value(Put(*database, "d", "1")); });
  EXPECT_EQ(written.wait_for(std::chrono::milliseconds(20)), std::future_status::timeout);
  table.Open();
  // The write resumes once the flush installs its table, before the cleanup.
  EXPECT_EQ(written.wait_for(std::chrono::seconds(10)), std::future_status::ready);
  cleanup.Open();
  writer.join();
  runner.join();
  file_system_.SetOperationHook({});

  EXPECT_TRUE(written.get().has_value());
  // The write switched while the first task was still scheduled, so that task
  // scheduled the next flush when it ended.
  EXPECT_EQ(ran, 2);
  EXPECT_EQ(executor_.RunAll(), 0);
  EXPECT_EQ(Numbers(FileType::Table).size(), 2U);
  EXPECT_EQ(Get(*database, "c"), Large());
}

TEST_F(DatabaseTest, ReturnsTheErrorOfANewLogThatCannotBeCreated) {
  const auto database = Open();
  Fill(*database, "a");
  // The new log fails to open, and then its directory entry fails to sync.
  for (const std::size_t failing : {std::size_t{0}, std::size_t{1}}) {
    SCOPED_TRACE(failing);
    FailInOperations(failing);
    const Status failed = Put(*database, "b", "1");
    ASSERT_FALSE(failed.has_value());
    EXPECT_EQ(failed.error().message(), "injected failure");
  }

  // Nothing stops later writes, which switch the log.
  ASSERT_TRUE(Put(*database, "b", "1").has_value());
  EXPECT_EQ(executor_.queued(), 1U);
  EXPECT_EQ(Get(*database, "b"), "1");
  EXPECT_EQ(Get(*database, "a"), Large());
  EXPECT_EQ(executor_.RunAll(), 1);
}

TEST_F(DatabaseTest, StopsWritesWhenTheOldLogCannotBeClosed) {
  const auto database = Open();
  Fill(*database, "a");
  FailInOperations(2);

  const Status failed = Put(*database, "b", "1");
  ASSERT_FALSE(failed.has_value());
  EXPECT_EQ(failed.error().message(), "injected failure");
  const Status later = Put(*database, "c", "1");
  ASSERT_FALSE(later.has_value());
  EXPECT_EQ(later.error().message(), "injected failure");
  EXPECT_EQ(Get(*database, "a"), Large());
  // The background error keeps the immutable memtable from being flushed.
  EXPECT_EQ(executor_.queued(), 0U);
  EXPECT_FALSE(database->WaitForBackgroundWork().has_value());
}

TEST_F(DatabaseTest, FlushesTheMemtableIntoATable) {
  const auto database = Open();
  ASSERT_TRUE(Put(*database, "a", "1").has_value());
  ASSERT_TRUE(Put(*database, "b", "2").has_value());
  const std::uint64_t old_log = Numbers(FileType::Log).back();

  ASSERT_TRUE(Flush(*database).has_value());

  EXPECT_EQ(Numbers(FileType::Table).size(), 1U);
  // The flushed memtable's log is obsolete and removed.
  EXPECT_FALSE(Exists(Numbered(old_log, ".log")));
  EXPECT_EQ(Numbers(FileType::Log).size(), 1U);
  EXPECT_EQ(Get(*database, "a"), "1");
  EXPECT_EQ(Scan(*database), (std::vector<std::string>{"a=1", "b=2"}));
  EXPECT_TRUE(database->WaitForBackgroundWork().has_value());

  // An empty memtable switches and flushes too, and writes no table.
  ASSERT_TRUE(Flush(*database).has_value());
  EXPECT_EQ(Numbers(FileType::Table).size(), 1U);
}

TEST_F(DatabaseTest, IteratesAcrossTheTablesAndBothMemtables) {
  const auto database = Open();
  ASSERT_TRUE(Put(*database, "a", "table").has_value());
  ASSERT_TRUE(Put(*database, "b", "table").has_value());
  ASSERT_TRUE(Flush(*database).has_value());
  ASSERT_TRUE(Put(*database, "c", "immutable").has_value());

  // The switch makes "c" immutable while its flush waits.
  std::optional<Status> flushed;
  std::thread flusher([&] { flushed = database->FlushMemTable(); });
  executor_.WaitForTask();
  ASSERT_TRUE(Put(*database, "d", "memtable").has_value());
  ASSERT_TRUE(Delete(*database, "a").has_value());

  EXPECT_EQ(Scan(*database), (std::vector<std::string>{"b=table", "c=immutable", "d=memtable"}));
  EXPECT_EQ(Get(*database, "c"), "immutable");
  // The flush waits for its table.
  EXPECT_FALSE(flushed.has_value());
  executor_.RunAll();
  flusher.join();
  EXPECT_TRUE(flushed->has_value());
  EXPECT_EQ(Scan(*database), (std::vector<std::string>{"b=table", "c=immutable", "d=memtable"}));
}

TEST_F(DatabaseTest, ReadsAtSnapshots) {
  const auto database = Open();
  ASSERT_TRUE(Put(*database, "a", "1").has_value());
  ASSERT_TRUE(Put(*database, "b", "1").has_value());
  const SequenceNumber first = database->GetSnapshot();
  const SequenceNumber again = database->GetSnapshot();
  ASSERT_TRUE(Put(*database, "a", "2").has_value());
  ASSERT_TRUE(Delete(*database, "b").has_value());
  ASSERT_TRUE(Flush(*database).has_value());

  EXPECT_EQ(first, again);
  EXPECT_EQ(Get(*database, "a", first), "1");
  EXPECT_EQ(Get(*database, "b", first), "1");
  EXPECT_EQ(Scan(*database, first), (std::vector<std::string>{"a=1", "b=1"}));
  EXPECT_EQ(Get(*database, "a"), "2");
  EXPECT_EQ(Scan(*database), (std::vector<std::string>{"a=2"}));
  // Each acquisition is released on its own.
  database->ReleaseSnapshot(first);
  EXPECT_EQ(Get(*database, "a", again), "1");
  database->ReleaseSnapshot(again);
}

TEST_F(DatabaseTest, PassesReadOptionsToTables) {
  BlockCache blocks(1 << 20);
  options_.block_cache = &blocks;
  const auto database = Open();
  ASSERT_TRUE(Put(*database, "a", "1").has_value());
  ASSERT_TRUE(Flush(*database).has_value());

  DatabaseReadOptions uncached;
  uncached.fill_cache = false;
  ASSERT_TRUE(database->Get(AsBytes("a"), uncached).has_value());
  const std::unique_ptr<DbIterator> iterator = database->NewIterator(uncached);
  ASSERT_TRUE(iterator->SeekToFirst().has_value());
  EXPECT_EQ(blocks.total_charge(), 0U);
  EXPECT_EQ(Get(*database, "a"), "1");
  EXPECT_GT(blocks.total_charge(), 0U);
}

TEST_F(DatabaseTest, RemovesObsoleteFilesWhenItOpens) {
  {
    const auto database = Open();
  }
  const std::uint64_t manifest = Numbers(FileType::Descriptor).back();
  file_system_.Write(directory_ / Numbered(1, ".log"), {});
  file_system_.Write(directory_ / Numbered(900, ".ldb"), {});
  file_system_.Write(directory_ / Numbered(901, ".dbtmp"), {});
  file_system_.Write(directory_ / "LOG", {});

  const auto database = Open();

  EXPECT_FALSE(Exists(Numbered(1, ".log")));
  EXPECT_FALSE(Exists(Numbered(900, ".ldb")));
  EXPECT_FALSE(Exists(Numbered(901, ".dbtmp")));
  EXPECT_TRUE(Exists("LOG"));
  EXPECT_TRUE(Exists("CURRENT"));
  EXPECT_TRUE(Exists("LOCK"));
  // Reopening wrote a new MANIFEST, so the old one is removed.
  EXPECT_FALSE(Exists("MANIFEST-" + Numbered(manifest, "")));
  EXPECT_EQ(Numbers(FileType::Descriptor).size(), 1U);
}

TEST_F(DatabaseTest, OpensWhenCleanupCannotListTheDirectory) {
  {
    const auto database = Open();
  }
  file_system_.Write(directory_ / Numbered(900, ".ldb"), {});
  // Recovery lists the directory first, and then cleanup does.
  int lists = 0;
  file_system_.SetOperationHook([&](std::string_view operation) -> Status {
    if (operation == "list db" && ++lists == 2) {
      return std::unexpected(Error::Io("injected failure"));
    }
    return {};
  });

  const auto database = Open();

  file_system_.SetOperationHook({});
  EXPECT_EQ(lists, 2);
  EXPECT_TRUE(Exists(Numbered(900, ".ldb")));
}

TEST_F(DatabaseTest, FlushWaitsForACommitThatReleasedTheMutex) {
  const auto database = Open();
  const std::string log = "append " + Numbered(Numbers(FileType::Log).back(), ".log");
  Gate gate;
  gate.Close([&](std::string_view operation) { return operation == log; });
  file_system_.SetOperationHook(std::ref(gate));
  std::optional<Status> written;
  std::thread writer([&] { written = Put(*database, "a", "1"); });
  gate.WaitUntilReached();

  // The writer commits with the mutex released; a flush may not switch the log
  // and the memtable under it. The pause gives the flush a chance to try.
  std::optional<Status> flushed;
  std::thread flusher([&] { flushed = database->FlushMemTable(); });
  std::this_thread::sleep_for(std::chrono::milliseconds(20));
  EXPECT_EQ(executor_.queued(), 0U);
  gate.Open();
  writer.join();
  executor_.WaitForTask();
  executor_.RunAll();
  flusher.join();
  file_system_.SetOperationHook({});

  EXPECT_TRUE(written->has_value());
  EXPECT_TRUE(flushed->has_value());
  // The switch came after the write, so the flushed table holds it.
  EXPECT_EQ(Numbers(FileType::Table).size(), 1U);
  EXPECT_EQ(Get(*database, "a"), "1");
}

TEST_F(DatabaseTest, StopsWritesAfterAFailedFlush) {
  const auto database = Open();
  ASSERT_TRUE(Put(*database, "a", "1").has_value());
  std::optional<Status> flushed;
  std::thread flusher([&] { flushed = database->FlushMemTable(); });
  executor_.WaitForTask();
  file_system_.SetOperationHook([](std::string_view operation) -> Status {
    if (OpensATable(operation)) {
      return std::unexpected(Error::Io("injected failure"));
    }
    return {};
  });
  executor_.RunAll();
  flusher.join();
  file_system_.SetOperationHook({});

  ASSERT_FALSE(flushed->has_value());
  EXPECT_EQ(flushed->error().message(), "injected failure");
  EXPECT_FALSE(Put(*database, "b", "1").has_value());
  EXPECT_EQ(Get(*database, "a"), "1");
  EXPECT_FALSE(database->WaitForBackgroundWork().has_value());
}

TEST_F(DatabaseTest, KeepsObsoleteFilesAfterABackgroundError) {
  const auto database = Open();
  ASSERT_TRUE(Put(*database, "a", "1").has_value());
  const std::uint64_t old_log = Numbers(FileType::Log).back();
  std::optional<Status> flushed;
  std::thread flusher([&] { flushed = database->FlushMemTable(); });
  executor_.WaitForTask();
  Gate gate;
  gate.Close(OpensATable);
  file_system_.SetOperationHook(std::ref(gate));
  std::thread runner([&] { executor_.RunAll(); });
  gate.WaitUntilReached();

  // A write fails while the flush writes its table with the mutex released.
  FailInOperations(0);
  const Status failed = Put(*database, "b", "1");
  gate.Open();
  runner.join();
  flusher.join();
  file_system_.SetOperationHook({});

  ASSERT_FALSE(failed.has_value());
  ASSERT_FALSE(flushed->has_value());
  EXPECT_EQ(flushed->error().message(), "injected failure");
  // The flush installed its table, but cleanup stopped, so the old log stays.
  EXPECT_EQ(Numbers(FileType::Table).size(), 1U);
  EXPECT_TRUE(Exists(Numbered(old_log, ".log")));
  EXPECT_EQ(Get(*database, "a"), "1");
}

TEST_F(DatabaseTest, SkipsBackgroundWorkAfterAnError) {
  const auto database = Open();
  Fill(*database, "a");
  ASSERT_TRUE(Put(*database, "b", "1").has_value());
  ASSERT_EQ(executor_.queued(), 1U);
  FailInOperations(0);
  ASSERT_FALSE(Put(*database, "c", "1").has_value());

  EXPECT_EQ(executor_.RunAll(), 1);
  EXPECT_TRUE(Numbers(FileType::Table).empty());
  EXPECT_EQ(Get(*database, "a"), Large());
}

TEST_F(DatabaseTest, StopsWritesAfterAFailedCommit) {
  const auto database = Open();
  FailInOperations(0);

  const Status failed = Put(*database, "a", "1");
  ASSERT_FALSE(failed.has_value());
  EXPECT_EQ(failed.error().message(), "injected failure");
  const Status later = Put(*database, "b", "1");
  ASSERT_FALSE(later.has_value());
  EXPECT_EQ(later.error().message(), "injected failure");
  EXPECT_EQ(Get(*database, "a"), "<none>");
  EXPECT_FALSE(database->FlushMemTable().has_value());
}

TEST_F(DatabaseTest, StopsWritesAfterAFailedSync) {
  const auto database = Open();
  ASSERT_TRUE(Put(*database, "a", "1").has_value());
  // A write appends its record twice and flushes it before it syncs.
  FailInOperations(3);

  const Status failed = Put(*database, "b", "1", true);
  ASSERT_FALSE(failed.has_value());
  EXPECT_EQ(failed.error().message(), "injected failure");
  EXPECT_FALSE(Put(*database, "c", "1").has_value());
  EXPECT_EQ(Get(*database, "a"), "1");
}

TEST_F(DatabaseTest, StopsWritesAfterACommitThrows) {
  const auto database = Open();
  file_system_.SetOperationHook([](std::string_view operation) -> Status {
    if (operation.starts_with("append ") && operation.ends_with(".log")) {
      throw std::runtime_error("thrown");
    }
    return {};
  });
  EXPECT_THROW(static_cast<void>(Put(*database, "a", "1")), std::runtime_error);
  file_system_.SetOperationHook({});

  const Status later = Put(*database, "b", "1");
  ASSERT_FALSE(later.has_value());
  EXPECT_EQ(later.error().code(), ErrorCode::Aborted);
}

TEST_F(DatabaseTest, StopsWritesWhenTheExecutorRejectsATask) {
  const auto database = Open();
  Fill(*database, "a");
  executor_.Reject(Error::Busy("rejected"));

  const Status failed = Put(*database, "b", "1");
  executor_.Reject(std::nullopt);

  ASSERT_FALSE(failed.has_value());
  EXPECT_EQ(failed.error().message(), "rejected");
  EXPECT_FALSE(Put(*database, "c", "1").has_value());
  EXPECT_EQ(executor_.queued(), 0U);
  EXPECT_EQ(Get(*database, "a"), Large());
  const Status waited = database->WaitForBackgroundWork();
  ASSERT_FALSE(waited.has_value());
  EXPECT_EQ(waited.error().message(), "rejected");
}

TEST_F(DatabaseTest, StopsWritesWhenSchedulingThrows) {
  const auto database = Open();
  Fill(*database, "a");
  executor_.Throw(true);

  const Status failed = Put(*database, "b", "1");
  executor_.Throw(false);

  ASSERT_FALSE(failed.has_value());
  EXPECT_EQ(failed.error().code(), ErrorCode::Aborted);
  EXPECT_EQ(executor_.queued(), 0U);
  EXPECT_EQ(Get(*database, "a"), Large());
  // No task stays marked as scheduled, so waiting and closing return.
  const Status waited = database->WaitForBackgroundWork();
  ASSERT_FALSE(waited.has_value());
  EXPECT_EQ(waited.error().code(), ErrorCode::Aborted);
}

TEST_F(DatabaseTest, IgnoresAnExceptionWhileClosingTheCurrentLog) {
  EXPECT_EXIT(
      {
        MemoryFileSystem file_system;
        ManualExecutor executor;
        ManualClock clock;
        DatabaseOptions options;
        options.file_system = &file_system;
        options.executor = &executor;
        options.clock = &clock;
        options.create_if_missing = true;
        options.write_buffer_size = 1;
        Result<std::unique_ptr<Database>> opened = Database::Open(options, "database");
        if (!opened.has_value()) {
          std::_Exit(1);
        }
        std::unique_ptr<Database> database = std::move(*opened);
        file_system.SetOperationHook([](std::string_view operation) -> Status {
          if (operation.starts_with("close ") && operation.ends_with(".log")) {
            throw std::runtime_error("thrown");
          }
          return {};
        });

        database.reset();
        std::_Exit(0);
      },
      testing::ExitedWithCode(0), "");
}

TEST_F(DatabaseTest, StopsWritesWhenTheExecutorRejectsTheNextTask) {
  const auto database = Open();
  const std::uint64_t first_log = Numbers(FileType::Log).back();
  Fill(*database, "a");
  ASSERT_TRUE(Put(*database, "b", "1").has_value());
  Fill(*database, "c");
  Gate gate;
  const std::string removal = "remove " + Numbered(first_log, ".log");
  gate.Close([&](std::string_view operation) { return operation == removal; });
  file_system_.SetOperationHook(std::ref(gate));
  std::optional<int> ran;
  std::thread runner([&] { ran = executor_.RunAll(); });
  gate.WaitUntilReached();

  // The memtable switches during the first task's cleanup, and the executor
  // rejects the next flush that the task schedules when it ends.
  ASSERT_TRUE(Put(*database, "d", "1").has_value());
  executor_.Reject(Error::Busy("rejected"));
  gate.Open();
  runner.join();
  executor_.Reject(std::nullopt);
  file_system_.SetOperationHook({});

  EXPECT_EQ(ran, 1);
  EXPECT_EQ(executor_.queued(), 0U);
  const Status later = Put(*database, "e", "1");
  ASSERT_FALSE(later.has_value());
  EXPECT_EQ(later.error().message(), "rejected");
  EXPECT_EQ(Get(*database, "d"), "1");
  const Status waited = database->WaitForBackgroundWork();
  ASSERT_FALSE(waited.has_value());
  EXPECT_EQ(waited.error().message(), "rejected");
}

TEST_F(DatabaseTest, RecordsAnExceptionWhileTheFlushWritesItsTable) {
  const auto database = Open();
  ASSERT_TRUE(Put(*database, "a", "1").has_value());
  std::optional<Status> flushed;
  std::thread flusher([&] { flushed = database->FlushMemTable(); });
  executor_.WaitForTask();
  file_system_.SetOperationHook([](std::string_view operation) -> Status {
    if (OpensATable(operation)) {
      throw std::runtime_error("thrown");
    }
    return {};
  });
  executor_.RunAll();
  flusher.join();
  file_system_.SetOperationHook({});

  ASSERT_FALSE(flushed->has_value());
  EXPECT_EQ(flushed->error().code(), ErrorCode::Aborted);
  EXPECT_EQ(Get(*database, "a"), "1");
}

TEST_F(DatabaseTest, RecordsAnExceptionWhileTheFlushAppliesItsEdit) {
  const auto database = Open();
  ASSERT_TRUE(Put(*database, "a", "1").has_value());
  std::optional<Status> flushed;
  std::thread flusher([&] { flushed = database->FlushMemTable(); });
  executor_.WaitForTask();
  // The MANIFEST is written with the mutex held.
  file_system_.SetOperationHook([](std::string_view operation) -> Status {
    if (operation.starts_with("append MANIFEST-")) {
      throw std::runtime_error("thrown");
    }
    return {};
  });
  executor_.RunAll();
  flusher.join();
  file_system_.SetOperationHook({});

  ASSERT_FALSE(flushed->has_value());
  EXPECT_EQ(flushed->error().code(), ErrorCode::Aborted);
}

TEST_F(DatabaseTest, WaitsForBackgroundWork) {
  const auto database = Open();
  EXPECT_TRUE(database->WaitForBackgroundWork().has_value());
  Fill(*database, "a");
  ASSERT_TRUE(Put(*database, "b", "1").has_value());

  std::optional<Status> waited;
  std::thread waiter([&] { waited = database->WaitForBackgroundWork(); });
  executor_.RunAll();
  waiter.join();
  EXPECT_TRUE(waited->has_value());
  EXPECT_EQ(Numbers(FileType::Table).size(), 1U);
}

TEST_F(DatabaseTest, ClosesWhileATaskIsPendingAndRecoversAfterward) {
  {
    auto database = Open();
    Fill(*database, "a");
    ASSERT_TRUE(Put(*database, "b", "1").has_value());
    ASSERT_EQ(executor_.queued(), 1U);
    EXPECT_EQ(CloseWithQueuedTasks(database), 1);
  }
  EXPECT_TRUE(Numbers(FileType::Table).empty());

  // Recovery replays both logs.
  const auto database = Open();
  EXPECT_EQ(Get(*database, "a"), Large());
  EXPECT_EQ(Get(*database, "b"), "1");
}

TEST_F(DatabaseTest, RejectsWritesPastTheLastSequence) {
  {
    const auto database = Open();
  }
  {
    const InternalKeyComparator comparator(BytewiseComparator());
    auto versions = VersionSet::Recover(file_system_, directory_, comparator);
    ASSERT_TRUE(versions.has_value());
    (*versions)->SetLastSequence(MaxSequenceNumber - 1);
    ASSERT_TRUE((*versions)->LogAndApply(VersionEdit()).has_value());
  }
  const auto database = Open();

  // The last sequence number is still free, and then none is.
  ASSERT_TRUE(Put(*database, "a", "1").has_value());
  const Status exhausted = Put(*database, "b", "1");
  ASSERT_FALSE(exhausted.has_value());
  EXPECT_EQ(exhausted.error().code(), ErrorCode::InvalidArgument);
  EXPECT_EQ(Get(*database, "a"), "1");
  EXPECT_EQ(Get(*database, "b"), "<none>");
}

TEST_F(DatabaseTest, ReturnsTheErrorsOfTableReads) {
  const auto database = Open();
  ASSERT_TRUE(Put(*database, "a", "1").has_value());
  ASSERT_TRUE(Flush(*database).has_value());
  const std::string read = "read " + Numbered(Numbers(FileType::Table).front(), ".ldb");
  file_system_.SetOperationHook([&](std::string_view operation) -> Status {
    if (operation == read) {
      return std::unexpected(Error::Io("injected failure"));
    }
    return {};
  });

  const auto value = database->Get(AsBytes("a"));
  file_system_.SetOperationHook({});

  ASSERT_FALSE(value.has_value());
  EXPECT_EQ(value.error().message(), "injected failure");
  EXPECT_EQ(Get(*database, "a"), "1");
}

// Flushes two tables from "a" to "z", which land in levels 2 and 1.
class DatabaseSeekTest : public DatabaseTest {
 protected:
  void FillLevels1And2(Database& database) {
    ASSERT_TRUE(Put(database, "z", "1").has_value());
    for (int i = 0; i < 2; ++i) {
      Fill(database, "a");
      ASSERT_TRUE(Put(database, "z", "2").has_value());
      ASSERT_TRUE(executor_.RunOne());
    }
    ASSERT_EQ(Levels(), "0 1 1 0 0 0 0");
  }

  // A read of a missing key between them searches the level-1 table and then
  // the level-2 table, which charges the level-1 table a seek.
  static void ReadPastTheLevel1Table(Database& database, int times) {
    for (int i = 0; i < times; ++i) {
      ASSERT_EQ(Get(database, "m"), "<none>");
    }
  }
};

TEST_F(DatabaseTest, CompactsLevel0InTheBackground) {
  const auto database = Open();
  WarmUp(*database);
  for (std::uint32_t i = 1; i < Level0CompactionTrigger; ++i) {
    Cycle(*database);
    EXPECT_EQ(executor_.queued(), 0U);
  }
  // The flush that makes the fourth level-0 file schedules their compaction.
  Cycle(*database);
  ASSERT_EQ(Levels(), "4 1 1 0 0 0 0");
  ASSERT_EQ(executor_.queued(), 1U);
  const std::vector<std::vector<std::uint64_t>> before = Files();

  ASSERT_TRUE(executor_.RunOne());

  // The inputs of levels 0 and 1 become one new level-1 table and are removed.
  const std::vector<std::vector<std::uint64_t>> after = Files();
  EXPECT_EQ(Levels(), "0 1 1 0 0 0 0");
  EXPECT_EQ(after[2], before[2]);
  for (const std::uint32_t level : {0U, 1U}) {
    for (const std::uint64_t number : before[level]) {
      EXPECT_FALSE(Exists(Numbered(number, ".ldb"))) << number;
      EXPECT_NE(after[1].front(), number);
    }
  }
  EXPECT_EQ(Numbers(FileType::Table).size(), 2U);
  EXPECT_EQ(executor_.queued(), 0U);
  EXPECT_EQ(Get(*database, "a"), Large());
  EXPECT_EQ(Get(*database, "z"), "1");
  EXPECT_EQ(Scan(*database).size(), 2U);

  // A second compaction replaces the first one's level-1 output. Its file
  // number stopped being pending after the first edit was installed, so
  // cleanup removes it now.
  const std::uint64_t first_output = after[1].front();
  for (std::uint32_t i = 0; i < Level0CompactionTrigger; ++i) {
    Cycle(*database);
  }
  ASSERT_TRUE(executor_.RunOne());
  EXPECT_EQ(Levels(), "0 1 1 0 0 0 0");
  EXPECT_FALSE(Exists(Numbered(first_output, ".ldb")));
  EXPECT_EQ(Numbers(FileType::Table).size(), 2U);
}

TEST_F(DatabaseTest, MovesALoneFileToTheNextLevelWithoutRewritingIt) {
  {
    const auto database = Open();
  }
  // Four level-0 files that overlap nothing make a compaction that moves one.
  AddTables({{0, {"a"}}, {0, {"c"}}, {0, {"e"}}, {0, {"g"}}});
  const std::vector<std::uint64_t> tables = Numbers(FileType::Table);
  ASSERT_EQ(tables.size(), 4U);
  {
    // The first move cannot write the MANIFEST, which stops writes.
    const auto database = Open();
    ASSERT_EQ(executor_.queued(), 1U);
    file_system_.SetOperationHook([](std::string_view operation) -> Status {
      if (operation.starts_with("append MANIFEST-")) {
        return std::unexpected(Error::Io("injected failure"));
      }
      return {};
    });
    ASSERT_TRUE(executor_.RunOne());
    file_system_.SetOperationHook({});
    const Status failed = database->WaitForBackgroundWork();
    ASSERT_FALSE(failed.has_value());
    EXPECT_EQ(failed.error().message(), "injected failure");
  }
  EXPECT_EQ(Levels(), "4 0 0 0 0 0 0");

  const auto database = Open();
  ASSERT_EQ(executor_.queued(), 1U);
  const std::size_t start = file_system_.operations().size();
  ASSERT_TRUE(executor_.RunOne());
  // The move writes no table and removes nothing.
  for (const std::string& operation : OperationsSince(start)) {
    EXPECT_FALSE(operation.ends_with(".ldb")) << operation;
    EXPECT_FALSE(operation.starts_with("remove ")) << operation;
  }
  EXPECT_EQ(Levels(), "3 1 0 0 0 0 0");
  EXPECT_EQ(Numbers(FileType::Table), tables);
  EXPECT_EQ(executor_.queued(), 0U);
  for (const std::string_view key : {"a", "c", "e", "g"}) {
    EXPECT_EQ(Get(*database, key), key);
  }
}

TEST_F(DatabaseTest, SlowsEachWriteOnceWhileLevel0HasEightFiles) {
  const auto database = Open();
  WarmUp(*database);
  for (std::size_t i = 0; i < Level0SlowdownWritesTrigger; ++i) {
    Cycle(*database);
  }
  ASSERT_EQ(Levels(), "8 1 1 0 0 0 0");
  EXPECT_EQ(Sleeps(), 0U);

  // Each write sleeps once, even one that then switches the memtable.
  ASSERT_TRUE(Put(*database, "b", "1").has_value());
  EXPECT_EQ(Sleeps(), 1U);
  Fill(*database, "c");
  ASSERT_TRUE(Put(*database, "d", "1").has_value());
  EXPECT_EQ(Sleeps(), 3U);
  ASSERT_TRUE(executor_.RunOne());
  ASSERT_EQ(Levels(), "9 1 1 0 0 0 0");

  // A forced flush does not sleep. The queued task flushes the memtable that
  // the flush switches once the switch closes the old log.
  Gate switched;
  switched.Close(ClosesTheLog());
  file_system_.SetOperationHook(std::ref(switched));
  std::future<Status> flushed =
      std::async(std::launch::async, [&] { return database->FlushMemTable(); });
  switched.WaitUntilReached();
  switched.Open();
  ASSERT_TRUE(executor_.RunOne());
  EXPECT_TRUE(flushed.get().has_value());
  file_system_.SetOperationHook({});
  EXPECT_EQ(Sleeps(), 3U);
  EXPECT_EQ(Levels(), "10 1 1 0 0 0 0");

  EXPECT_EQ(executor_.RunAll(), 1);
  EXPECT_EQ(Levels(), "0 1 1 0 0 0 0");
}

TEST_F(DatabaseTest, StopsWritesThatNeedAMemtableWhileLevel0HasTwelveFiles) {
  const auto database = Open();
  WarmUp(*database);
  for (std::size_t i = 0; i < Level0StopWritesTrigger; ++i) {
    Cycle(*database);
  }
  ASSERT_EQ(Levels(), "12 1 1 0 0 0 0");

  // This write has room, but the next one needs a new memtable, so it waits
  // until the queued compaction empties level 0.
  Fill(*database, "b");
  std::future<Status> written =
      std::async(std::launch::async, [&] { return Put(*database, "c", "1"); });
  EXPECT_EQ(written.wait_for(std::chrono::milliseconds(20)), std::future_status::timeout);
  ASSERT_TRUE(executor_.RunOne());
  EXPECT_EQ(written.wait_for(std::chrono::seconds(10)), std::future_status::ready);
  EXPECT_TRUE(written.get().has_value());
  EXPECT_EQ(Levels(), "0 1 1 0 0 0 0");

  EXPECT_EQ(executor_.RunAll(), 1);
  EXPECT_EQ(Get(*database, "b"), Large());
  EXPECT_EQ(Get(*database, "c"), "1");
}

TEST_F(DatabaseTest, SchedulesACompactionWhenItOpens) {
  {
    auto database = Open();
    WarmUp(*database);
    for (std::uint32_t i = 0; i < Level0CompactionTrigger; ++i) {
      Cycle(*database);
    }
    EXPECT_EQ(CloseWithQueuedTasks(database), 1);
  }
  ASSERT_EQ(Levels(), "4 1 1 0 0 0 0");

  // A rejected task fails the open.
  executor_.Reject(Error::Busy("rejected"));
  const Result<std::unique_ptr<Database>> rejected = TryOpen();
  executor_.Reject(std::nullopt);
  ASSERT_FALSE(rejected.has_value());
  EXPECT_EQ(rejected.error().message(), "rejected");

  const auto database = Open();
  ASSERT_EQ(executor_.queued(), 1U);
  ASSERT_TRUE(executor_.RunOne());
  EXPECT_EQ(Levels(), "0 1 1 0 0 0 0");
  EXPECT_EQ(Get(*database, "a"), Large());
}

TEST_F(DatabaseTest, StopsWritesAfterACompactionFails) {
  {
    auto database = Open();
    WarmUp(*database);
    for (std::uint32_t i = 0; i < Level0CompactionTrigger; ++i) {
      Cycle(*database);
    }
    EXPECT_EQ(CloseWithQueuedTasks(database), 1);
  }
  const std::vector<std::uint64_t> tables = Numbers(FileType::Table);
  // The compaction cannot open its output, sync the directory after its
  // outputs, or write the MANIFEST.
  for (const std::string_view failing : {"open_writable ", "sync_directory ", "append MANIFEST-"}) {
    SCOPED_TRACE(failing);
    const auto database = Open();
    ASSERT_EQ(executor_.queued(), 1U);
    file_system_.SetOperationHook([failing](std::string_view operation) -> Status {
      if (operation.starts_with(failing)) {
        return std::unexpected(Error::Io("injected failure"));
      }
      return {};
    });
    ASSERT_TRUE(executor_.RunOne());
    file_system_.SetOperationHook({});
    const Status later = Put(*database, "b", "1");
    ASSERT_FALSE(later.has_value());
    EXPECT_EQ(later.error().message(), "injected failure");
    EXPECT_EQ(Levels(), "4 1 1 0 0 0 0");
    EXPECT_EQ(Get(*database, "a"), Large());
  }

  // Opening removes the failed outputs, and the compaction runs again.
  const auto database = Open();
  EXPECT_EQ(Numbers(FileType::Table), tables);
  ASSERT_TRUE(executor_.RunOne());
  EXPECT_EQ(Levels(), "0 1 1 0 0 0 0");
  EXPECT_EQ(Get(*database, "a"), Large());
}

TEST_F(DatabaseSeekTest, CompactsAFileThatReadsSeekPastTooOften) {
  const auto database = Open();
  FillLevels1And2(*database);
  // The table's budget of 100 seeks runs out at the hundredth read.
  ReadPastTheLevel1Table(*database, 99);
  EXPECT_EQ(executor_.queued(), 0U);
  ReadPastTheLevel1Table(*database, 1);
  ASSERT_EQ(executor_.queued(), 1U);

  // The compaction merges the level-1 table into level 2.
  ASSERT_TRUE(executor_.RunOne());
  EXPECT_EQ(Levels(), "0 0 1 0 0 0 0");
  EXPECT_EQ(executor_.queued(), 0U);
  EXPECT_EQ(Get(*database, "a"), Large());
  EXPECT_EQ(Get(*database, "z"), "2");
}

TEST_F(DatabaseSeekTest, CompactsAFileThatIteratorSamplesSeekPastTooOften) {
  const auto database = Open();
  FillLevels1And2(*database);
  ReadPastTheLevel1Table(*database, 99);
  EXPECT_EQ(executor_.queued(), 0U);

  // Each pass reads at least the newest "a", so 64 passes read more than two
  // sampling periods, each below 2 MiB. Every key is in both tables, so each
  // sample charges the level-1 table, whose budget the first runs out.
  ScanRepeatedly(*database, 64);
  ASSERT_EQ(executor_.queued(), 1U);
  ASSERT_TRUE(executor_.RunOne());
  EXPECT_EQ(Levels(), "0 0 1 0 0 0 0");

  // Samples of keys in one table charge nothing.
  ScanRepeatedly(*database, 64);
  EXPECT_EQ(executor_.queued(), 0U);
}

TEST_F(DatabaseTest, AbortsACompactionWhenTheDatabaseCloses) {
  auto database = Open();
  WarmUp(*database);
  for (std::uint32_t i = 0; i < Level0CompactionTrigger; ++i) {
    Cycle(*database);
  }
  ASSERT_EQ(executor_.queued(), 1U);
  const std::vector<std::uint64_t> tables = Numbers(FileType::Table);
  // Hold the compaction at its output's open, which follows the check before
  // its first entry, and then the destructor once it marks the database as
  // closing.
  Gate output;
  output.Close(OpensATable);
  Gate closing;
  closing.Close(ClosesTheLog());
  file_system_.SetOperationHook([&](std::string_view operation) {
    static_cast<void>(output(operation));
    return closing(operation);
  });
  std::thread runner([&] { executor_.RunOne(); });
  output.WaitUntilReached();
  std::thread closer([&] { database.reset(); });
  closing.WaitUntilReached();
  // Both threads wait in the hook, before their operations are logged.
  const std::size_t start = file_system_.operations().size();
  closing.Open();
  output.Open();
  closer.join();
  runner.join();
  file_system_.SetOperationHook({});

  // The compaction stopped before its next entry, so it never finished its
  // output, and the next open removes the output and compacts again.
  for (const std::string& operation : OperationsSince(start)) {
    EXPECT_FALSE(operation.starts_with("sync_directory ")) << operation;
  }
  EXPECT_EQ(Levels(), "4 1 1 0 0 0 0");
  database = Open();
  EXPECT_EQ(Numbers(FileType::Table), tables);
  ASSERT_TRUE(executor_.RunOne());
  EXPECT_EQ(Levels(), "0 1 1 0 0 0 0");
  EXPECT_EQ(Get(*database, "a"), Large());
}

TEST_F(DatabaseTest, DiscardsACompactionThatFinishesWhileTheDatabaseCloses) {
  auto database = Open();
  WarmUp(*database);
  for (std::uint32_t i = 0; i < Level0CompactionTrigger; ++i) {
    Cycle(*database);
  }
  ASSERT_EQ(executor_.queued(), 1U);
  const std::vector<std::uint64_t> tables = Numbers(FileType::Table);
  // Hold the compaction at its directory sync after its last entry, and then
  // the destructor once it marks the database as closing.
  Gate synced;
  synced.Close([](std::string_view operation) { return operation.starts_with("sync_directory "); });
  Gate closing;
  closing.Close(ClosesTheLog());
  file_system_.SetOperationHook([&](std::string_view operation) {
    static_cast<void>(synced(operation));
    return closing(operation);
  });
  std::thread runner([&] { executor_.RunOne(); });
  synced.WaitUntilReached();
  std::thread closer([&] { database.reset(); });
  closing.WaitUntilReached();
  closing.Open();
  synced.Open();
  closer.join();
  runner.join();
  file_system_.SetOperationHook({});

  // The finished compaction was not applied, so the next open removes its
  // output and compacts again.
  EXPECT_EQ(Levels(), "4 1 1 0 0 0 0");
  database = Open();
  EXPECT_EQ(Numbers(FileType::Table), tables);
  ASSERT_TRUE(executor_.RunOne());
  EXPECT_EQ(Levels(), "0 1 1 0 0 0 0");
  EXPECT_EQ(Get(*database, "a"), Large());
}

TEST_F(DatabaseTest, KeepsWhatLiveSnapshotsReadThroughCompactions) {
  const auto database = Open();
  WarmUp(*database);
  ASSERT_TRUE(Put(*database, "k", "1").has_value());
  const SequenceNumber first = database->GetSnapshot();
  ASSERT_TRUE(Put(*database, "k", "2").has_value());
  const SequenceNumber second = database->GetSnapshot();
  const SequenceNumber again = database->GetSnapshot();
  ASSERT_TRUE(Put(*database, "k", "3").has_value());

  // Every snapshot reads its value after a compaction.
  CompactLevel0(*database);
  EXPECT_EQ(Get(*database, "k", first), "1");
  EXPECT_EQ(Seek(*database, "k", first), "1");
  EXPECT_EQ(Get(*database, "k", second), "2");
  EXPECT_EQ(Seek(*database, "k", second), "2");
  EXPECT_EQ(Get(*database, "k"), "3");
  EXPECT_EQ(Seek(*database, "k"), "3");

  // Once the oldest is released, the next compaction drops what only it read,
  // as a read at its sequence shows.
  database->ReleaseSnapshot(first);
  CompactLevel0(*database);
  EXPECT_EQ(Get(*database, "k", first), "<none>");
  EXPECT_EQ(Get(*database, "k", second), "2");
  EXPECT_EQ(Seek(*database, "k", second), "2");

  // Of two snapshots at one sequence, the one still held keeps its value.
  database->ReleaseSnapshot(second);
  CompactLevel0(*database);
  EXPECT_EQ(Get(*database, "k", again), "2");
  EXPECT_EQ(Seek(*database, "k", again), "2");

  // Without snapshots, a compaction keeps only the newest value.
  database->ReleaseSnapshot(again);
  CompactLevel0(*database);
  EXPECT_EQ(Get(*database, "k", again), "<none>");
  EXPECT_EQ(Get(*database, "k"), "3");
  EXPECT_EQ(Seek(*database, "k"), "3");
}

TEST_F(DatabaseTest, FlushesDuringACompactionSoThatWritersProceed) {
  const auto database = Open();
  WarmUp(*database);
  for (std::uint32_t i = 0; i < Level0CompactionTrigger; ++i) {
    Cycle(*database);
  }
  ASSERT_EQ(executor_.queued(), 1U);
  // Hold the compaction at its output's open.
  Gate output;
  output.Close(OpensATable);
  Gate cleanup;
  file_system_.SetOperationHook([&](std::string_view operation) {
    static_cast<void>(output(operation));
    return cleanup(operation);
  });
  std::optional<bool> ran;
  std::thread runner([&] { ran = executor_.RunOne(); });
  output.WaitUntilReached();

  // A switch makes an immutable memtable, and a write that needs another
  // switch waits for it.
  const std::uint64_t old_log = Numbers(FileType::Log).back();
  Fill(*database, "b");
  ASSERT_TRUE(Put(*database, "c", "1").has_value());
  Fill(*database, "d");
  std::future<Status> written =
      std::async(std::launch::async, [&] { return Put(*database, "e", "1"); });
  EXPECT_EQ(written.wait_for(std::chrono::milliseconds(20)), std::future_status::timeout);

  // The compaction flushes the immutable memtable before its next entry, and
  // the write proceeds while the compaction is held in that flush's cleanup.
  const std::string removal = "remove " + Numbered(old_log, ".log");
  cleanup.Close([removal](std::string_view operation) { return operation == removal; });
  output.Open();
  cleanup.WaitUntilReached();
  EXPECT_EQ(written.wait_for(std::chrono::seconds(10)), std::future_status::ready);
  cleanup.Open();
  runner.join();
  file_system_.SetOperationHook({});

  EXPECT_TRUE(written.get().has_value());
  EXPECT_EQ(ran, true);
  // The writer's switch made another immutable memtable, which the compaction
  // flushed before its next entry too. Neither flush was among its inputs.
  EXPECT_EQ(Levels(), "2 1 1 0 0 0 0");
  EXPECT_EQ(executor_.queued(), 0U);
  EXPECT_EQ(Get(*database, "b"), Large());
  EXPECT_EQ(Get(*database, "d"), Large());
  EXPECT_EQ(Get(*database, "e"), "1");
}

TEST_F(DatabaseTest, StopsACompactionWhoseFlushFails) {
  const auto database = Open();
  WarmUp(*database);
  for (std::uint32_t i = 0; i < Level0CompactionTrigger; ++i) {
    Cycle(*database);
  }
  ASSERT_EQ(executor_.queued(), 1U);
  // Hold the compaction at its output's open, and fail the next table to open,
  // which is the flush's.
  Gate output;
  output.Close(OpensATable);
  std::atomic<int> opened = 0;
  file_system_.SetOperationHook([&](std::string_view operation) -> Status {
    static_cast<void>(output(operation));
    if (OpensATable(operation) && ++opened == 2) {
      return std::unexpected(Error::Io("injected failure"));
    }
    return {};
  });
  std::thread runner([&] { executor_.RunOne(); });
  output.WaitUntilReached();
  Fill(*database, "b");
  ASSERT_TRUE(Put(*database, "c", "1").has_value());
  output.Open();
  runner.join();
  file_system_.SetOperationHook({});

  // The failed flush stops the compaction, and neither is applied.
  const Status failed = database->WaitForBackgroundWork();
  ASSERT_FALSE(failed.has_value());
  EXPECT_EQ(failed.error().message(), "injected failure");
  EXPECT_EQ(Levels(), "4 1 1 0 0 0 0");
  EXPECT_FALSE(Put(*database, "d", "1").has_value());
  EXPECT_EQ(Get(*database, "b"), Large());
  EXPECT_EQ(Get(*database, "c"), "1");
}

}  // namespace
}  // namespace modern_leveldb
