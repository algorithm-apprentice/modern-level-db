#ifndef MODERN_LEVELDB_TESTS_UNIT_ENGINE_DATABASE_TEST_SUPPORT_H_
#define MODERN_LEVELDB_TESTS_UNIT_ENGINE_DATABASE_TEST_SUPPORT_H_

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
#include <limits>
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
#include "engine/database.h"
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
namespace database_test {

using test_support::ManualClock;
using test_support::ManualExecutor;
using test_support::MemoryFileSystem;

inline std::string Text(ByteView bytes) { return std::string(AsStringView(bytes)); }

inline std::string Numbered(std::uint64_t number, std::string_view suffix) {
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

class BlockingComparator final : public Comparator {
public:
    int Compare(ByteView left, ByteView right) const noexcept override {
        {
            std::unique_lock lock(mutex_);
            if (!open_ && !reached_ &&
                (AsStringView(left) == target_ || AsStringView(right) == target_)) {
                reached_ = true;
                changed_.notify_all();
                changed_.wait(lock, [this] { return open_; });
            }
        }
        return BytewiseComparator().Compare(left, right);
    }
    std::string_view Name() const noexcept override { return "test.Blocking"; }
    void FindShortestSeparator(std::vector<std::byte>&, ByteView) const override {}
    void FindShortSuccessor(std::vector<std::byte>&) const override {}

    void Close(std::string target) {
        const std::lock_guard lock(mutex_);
        target_ = std::move(target);
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

private:
    mutable std::mutex mutex_;
    mutable std::condition_variable changed_;
    mutable std::string target_;
    mutable bool open_ = true;
    mutable bool reached_ = false;
};

inline bool OpensATable(std::string_view operation) {
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

    Result<std::unique_ptr<DatabaseEngine>> TryOpen() {
        return DatabaseEngine::Open(options_, directory_);
    }

    std::unique_ptr<DatabaseEngine> Open() {
        Result<std::unique_ptr<DatabaseEngine>> database = TryOpen();
        EXPECT_TRUE(database.has_value()) << database.error().ToString();
        return database.has_value() ? std::move(*database) : nullptr;
    }

    static Status Put(DatabaseEngine& database, std::string_view key, std::string_view value,
                      bool sync = false) {
        EncodedWriteBatch batch;
        batch.Put(AsBytes(key), AsBytes(value));
        return database.Write(batch, sync);
    }

    static Status Delete(DatabaseEngine& database, std::string_view key) {
        EncodedWriteBatch batch;
        batch.Delete(AsBytes(key));
        return database.Write(batch, false);
    }

    // The value, "<none>" for a missing key, or "<error>".
    static std::string Get(DatabaseEngine& database, std::string_view key,
                           std::optional<SequenceNumber> snapshot = std::nullopt) {
        DatabaseEngineReadOptions options;
        options.snapshot = snapshot;
        std::vector<std::byte> value;
        const auto found = database.Get(AsBytes(key), value, options);
        if (!found.has_value()) {
            return "<error>";
        }
        return *found ? Text(value) : "<none>";
    }

    static std::vector<std::string> Scan(DatabaseEngine& database,
                                         std::optional<SequenceNumber> snapshot = std::nullopt) {
        DatabaseEngineReadOptions options;
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
    static void Fill(DatabaseEngine& database, std::string_view key) {
        ASSERT_TRUE(Put(database, key, Large()).has_value());
    }

    // The numbers of the directory's files of the type.
    std::vector<std::uint64_t> Numbers(FileType type) const {
        const Result<std::vector<std::filesystem::path>> names =
            file_system_.ListDirectory(directory_);
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
    Status Flush(DatabaseEngine& database) {
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
                memtable.Add(++sequence, ValueKind::Value, AsBytes(key), AsBytes(key));
            }
            auto built = BuildTable(file_system_, directory_, comparator, TableBuilderOptions(),
                                    cache, memtable, (*versions)->NewFileNumber());
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
    void Cycle(DatabaseEngine& database) {
        ASSERT_TRUE(Put(database, "z", "1").has_value());
        Fill(database, "a");
        EncodedWriteBatch empty;
        ASSERT_TRUE(database.Write(empty, false).has_value());
        ASSERT_TRUE(executor_.RunOne());
    }

    // A flush lands in level 0 only if it overlaps level 0 or level 1, so two
    // flushes fill levels 2 and 1 first, and each later cycle adds a level-0
    // file.
    void WarmUp(DatabaseEngine& database) {
        Cycle(database);
        Cycle(database);
        ASSERT_EQ(Levels(), "0 1 1 0 0 0 0");
    }

    // Adds level-0 files to a warmed-up database until a compaction of them is
    // queued, and runs it.
    void CompactLevel0(DatabaseEngine& database) {
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
    int CloseWithQueuedTasks(std::unique_ptr<DatabaseEngine>& database) {
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
    static std::string Seek(DatabaseEngine& database, std::string_view key,
                            std::optional<SequenceNumber> snapshot = std::nullopt) {
        DatabaseEngineReadOptions options;
        options.snapshot = snapshot;
        const std::unique_ptr<DbIterator> iterator = database.NewIterator(options);
        EXPECT_TRUE(iterator->Seek(AsBytes(key)).has_value());
        if (!iterator->valid() || Text(iterator->key()) != key) {
            return "<none>";
        }
        return Text(iterator->value());
    }

    // Reads every entry, forward, in each of the passes.
    static void ScanRepeatedly(DatabaseEngine& database, int passes) {
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
    DatabaseEngineOptions options_;
    const std::filesystem::path directory_ = std::filesystem::path("db");
};

}  // namespace database_test
}  // namespace modern_leveldb

#endif  // MODERN_LEVELDB_TESTS_UNIT_ENGINE_DATABASE_TEST_SUPPORT_H_
