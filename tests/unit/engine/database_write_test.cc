#include "database_test_support.h"

namespace modern_leveldb {
namespace database_test {

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
    auto* before = database->GetSnapshot();
    EncodedWriteBatch empty;
    ASSERT_TRUE(database->Write(empty, false).has_value());
    auto* after = database->GetSnapshot();
    EXPECT_EQ(after->sequence(), before->sequence());
    EXPECT_EQ(before->sequence(), 4U);
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

TEST_F(DatabaseTest, CanSwitchLogsWithoutTheWalCreationBarrier) {
    options_.sync_wal_creation = false;
    const auto database = Open();
    const std::uint64_t old_log = Numbers(FileType::Log).back();
    Fill(*database, "a");

    const std::size_t start = file_system_.operations().size();
    ASSERT_TRUE(Put(*database, "b", "1").has_value());
    const std::vector<std::string> operations = OperationsSince(start);
    ASSERT_GE(operations.size(), 3U);
    const std::string new_log = operations[0].substr(std::string("open_writable ").size());
    EXPECT_EQ(operations[0], "open_writable " + new_log);
    EXPECT_EQ(operations[1], "close " + Numbered(old_log, ".log"));
    EXPECT_EQ(operations[2], "append " + new_log);
    EXPECT_EQ(std::ranges::count(operations, "sync_directory db"), 0);
    EXPECT_EQ(executor_.RunAll(), 1);
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
    const auto before = database->GetState();
    ASSERT_TRUE(before.has_value());
    // The new log fails to open, and then its directory entry fails to sync.
    for (const std::size_t failing : {std::size_t{0}, std::size_t{1}}) {
        SCOPED_TRACE(failing);
        const std::size_t start = file_system_.operations().size();
        FailInOperations(failing);
        const Status failed = Put(*database, "b", "1");
        ASSERT_FALSE(failed.has_value());
        EXPECT_EQ(failed.error().code(), ErrorCode::Io);
        EXPECT_EQ(failed.error().message(), "injected failure");
        const auto after = database->GetState();
        ASSERT_TRUE(after.has_value());
        EXPECT_EQ(after->last_sequence, before->last_sequence);
        EXPECT_EQ(after->mutable_memtable_bytes, before->mutable_memtable_bytes);
        EXPECT_FALSE(after->immutable_memtable_bytes.has_value());
        EXPECT_FALSE(after->sticky_error.has_value());
        EXPECT_FALSE(after->background_work_scheduled);
        for (const auto& operation : OperationsSince(start)) {
            EXPECT_FALSE(operation.starts_with("close ")) << operation;
            EXPECT_FALSE(operation.starts_with("append ")) << operation;
        }
    }

    // Nothing stops later writes, which switch the log.
    ASSERT_TRUE(Put(*database, "b", "1").has_value());
    EXPECT_EQ(Numbers(FileType::Log), (std::vector<std::uint64_t>{2U, 4U, 5U}));
    EXPECT_EQ(executor_.queued(), 1U);
    EXPECT_EQ(Get(*database, "b"), "1");
    EXPECT_EQ(Get(*database, "a"), Large());
    EXPECT_EQ(executor_.RunAll(), 1);
}

TEST_F(DatabaseTest, StopsWritesWhenTheOldLogCannotBeClosed) {
    const auto database = Open();
    Fill(*database, "a");
    const auto before = database->GetState();
    ASSERT_TRUE(before.has_value());
    const std::size_t start = file_system_.operations().size();
    FailInOperations(2);

    const Status failed = Put(*database, "b", "1");
    ASSERT_FALSE(failed.has_value());
    EXPECT_EQ(failed.error().code(), ErrorCode::Io);
    EXPECT_EQ(failed.error().message(), "injected failure");
    const auto after = database->GetState();
    ASSERT_TRUE(after.has_value());
    EXPECT_EQ(after->last_sequence, before->last_sequence);
    ASSERT_TRUE(after->immutable_memtable_bytes.has_value());
    EXPECT_EQ(*after->immutable_memtable_bytes, before->mutable_memtable_bytes);
    EXPECT_LT(after->mutable_memtable_bytes, before->mutable_memtable_bytes);
    ASSERT_TRUE(after->sticky_error.has_value());
    EXPECT_EQ(after->sticky_error->code(), ErrorCode::Io);
    EXPECT_EQ(after->sticky_error->message(), "injected failure");
    for (const auto& operation : OperationsSince(start)) {
        EXPECT_FALSE(operation.starts_with("append ")) << operation;
    }
    const Status later = Put(*database, "c", "1");
    ASSERT_FALSE(later.has_value());
    EXPECT_EQ(later.error().code(), ErrorCode::Io);
    EXPECT_EQ(later.error().message(), "injected failure");
    EXPECT_EQ(Get(*database, "a"), Large());
    // The sticky error keeps the immutable memtable from being flushed.
    EXPECT_EQ(executor_.queued(), 0U);
    EXPECT_FALSE(database->WaitForBackgroundWork().has_value());
}

TEST_F(DatabaseTest, StopsWritesWhenClosingTheOldLogThrows) {
    const auto database = Open();
    Fill(*database, "a");
    const auto before = database->GetState();
    ASSERT_TRUE(before.has_value());
    const std::size_t start = file_system_.operations().size();
    file_system_.SetOperationHook([](std::string_view operation) -> Status {
        if (operation.starts_with("close ") && operation.ends_with(".log")) {
            throw std::runtime_error("thrown");
        }
        return {};
    });

    EXPECT_THROW(static_cast<void>(Put(*database, "b", "1")), std::runtime_error);
    file_system_.SetOperationHook({});

    const auto after = database->GetState();
    ASSERT_TRUE(after.has_value());
    EXPECT_EQ(after->last_sequence, before->last_sequence);
    EXPECT_EQ(after->mutable_memtable_bytes, before->mutable_memtable_bytes);
    EXPECT_FALSE(after->immutable_memtable_bytes.has_value());
    ASSERT_TRUE(after->sticky_error.has_value());
    EXPECT_EQ(after->sticky_error->code(), ErrorCode::Aborted);
    EXPECT_EQ(after->sticky_error->message(), "old WAL close threw during rotation");
    for (const auto& operation : OperationsSince(start)) {
        EXPECT_FALSE(operation.starts_with("append ")) << operation;
    }
    const Status later = Put(*database, "c", "1");
    ASSERT_FALSE(later.has_value());
    EXPECT_EQ(later.error().code(), ErrorCode::Aborted);
    EXPECT_EQ(later.error().message(), "old WAL close threw during rotation");
    EXPECT_EQ(Get(*database, "a"), Large());
    EXPECT_EQ(executor_.queued(), 0U);
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

TEST_F(DatabaseTest, StopsWritesAfterAFailedCommit) {
    const auto database = Open();
    auto* before = database->GetSnapshot();
    FailInOperations(0);

    const Status failed = Put(*database, "a", "1");
    ASSERT_FALSE(failed.has_value());
    EXPECT_EQ(failed.error().message(), "injected failure");
    const Status later = Put(*database, "b", "1");
    ASSERT_FALSE(later.has_value());
    EXPECT_EQ(later.error().message(), "injected failure");
    EXPECT_EQ(Get(*database, "a"), "<none>");
    auto* after = database->GetSnapshot();
    EXPECT_EQ(after->sequence(), before->sequence());
    database->ReleaseSnapshot(before);
    database->ReleaseSnapshot(after);
    EXPECT_FALSE(database->FlushMemTable().has_value());
}

TEST_F(DatabaseTest, StopsWritesAfterAFailedSync) {
    const auto database = Open();
    ASSERT_TRUE(Put(*database, "a", "1").has_value());
    auto* before = database->GetSnapshot();
    // A write appends its record twice and flushes it before it syncs.
    FailInOperations(3);

    const Status failed = Put(*database, "b", "1", true);
    ASSERT_FALSE(failed.has_value());
    EXPECT_EQ(failed.error().message(), "injected failure");
    EXPECT_FALSE(Put(*database, "c", "1").has_value());
    EXPECT_EQ(Get(*database, "a"), "1");
    auto* after = database->GetSnapshot();
    EXPECT_EQ(after->sequence(), before->sequence());
    database->ReleaseSnapshot(before);
    database->ReleaseSnapshot(after);
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

TEST_F(DatabaseTest, WritesProceedWhileManifestSyncIsBlocked) {
    const auto database = Open();
    Fill(*database, "a");
    ASSERT_TRUE(Put(*database, "b", "1").has_value());
    Gate manifest;
    manifest.Close([](std::string_view operation) { return operation == "sync MANIFEST-000001"; });
    file_system_.SetOperationHook(std::ref(manifest));
    std::thread runner([&] { EXPECT_TRUE(executor_.RunOne()); });
    manifest.WaitUntilReached();

    std::future<Status> written =
        std::async(std::launch::async, [&] { return Put(*database, "c", "1"); });
    const std::future_status progress = written.wait_for(std::chrono::seconds(10));
    DatabaseEngine::SnapshotRegistration* snapshot = nullptr;
    if (progress == std::future_status::ready) {
        snapshot = database->GetSnapshot();
    }
    manifest.Open();
    runner.join();
    file_system_.SetOperationHook({});

    ASSERT_EQ(progress, std::future_status::ready);
    EXPECT_TRUE(written.get().has_value());
    ASSERT_NE(snapshot, nullptr);
    EXPECT_EQ(snapshot->sequence(), 3U);
    database->ReleaseSnapshot(snapshot);
    EXPECT_EQ(Get(*database, "c"), "1");
}

TEST_F(DatabaseTest, SlowsEachWriteOnceWhileLevel0HasEightFiles) {
    const auto database = Open();
    WarmUp(*database);
    for (std::size_t i = 0; i < WriteDelayLevel0FileCountThreshold; ++i) {
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
    for (std::size_t i = 0; i < WriteStallLevel0FileCountThreshold; ++i) {
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

}  // namespace database_test
}  // namespace modern_leveldb
