#include "engine/database.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
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

#include "format/internal_key.h"
#include "format/write_batch.h"
#include "metadata/filenames.h"
#include "metadata/version_edit.h"
#include "metadata/version_set.h"
#include "modern_leveldb/base/bytes.h"
#include "modern_leveldb/base/comparator.h"
#include "modern_leveldb/base/result.h"
#include "support/manual_executor.h"
#include "support/memory_file_system.h"
#include "table/table.h"

namespace modern_leveldb {
namespace {

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

  MemoryFileSystem file_system_;
  ManualExecutor executor_;
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
    std::thread closer([&] { database.reset(); });
    // The pause lets the closer mark the database as closing, so that the
    // task does no work.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_EQ(executor_.RunAll(), 1);
    closer.join();
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

}  // namespace
}  // namespace modern_leveldb
