#include "database_test_support.h"

namespace modern_leveldb {
namespace database_test {

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

TEST_F(DatabaseTest, StopsWritesAfterAFailedFlush) {
    const auto database = Open();
    ASSERT_TRUE(Put(*database, "a", "1").has_value());
    std::optional<Status> flushed;
    std::thread flusher([&] { flushed = database->FlushMemTable(); });
    executor_.WaitForTask();
    const std::size_t start = file_system_.operations().size();
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
    const auto state = database->GetState();
    ASSERT_TRUE(state.has_value());
    EXPECT_TRUE(state->immutable_memtable_bytes.has_value());
    EXPECT_EQ(state->protected_output_count, 0U);
    EXPECT_FALSE(state->background_work_scheduled);
    ASSERT_TRUE(state->sticky_error.has_value());
    EXPECT_EQ(state->sticky_error->message(), "injected failure");
    for (const std::string& operation : OperationsSince(start)) {
        EXPECT_FALSE(operation.starts_with("append MANIFEST-")) << operation;
        EXPECT_FALSE(operation.starts_with("remove ")) << operation;
    }
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
    const auto state = database->GetState();
    ASSERT_TRUE(state.has_value());
    EXPECT_FALSE(state->immutable_memtable_bytes.has_value());
    EXPECT_EQ(state->protected_output_count, 0U);
    ASSERT_TRUE(state->sticky_error.has_value());
    EXPECT_EQ(state->sticky_error->message(), failed.error().message());
    std::size_t installed_files = 0;
    for (const DatabaseEngineLevelState& level : state->levels) {
        installed_files += level.file_count;
    }
    EXPECT_EQ(installed_files, 1U);
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
    // The background call reacquires the mutex before it records the exception.
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

TEST_F(DatabaseTest, DiscardsAFlushThatFinishesWhileTheDatabaseCloses) {
    auto database = Open();
    Fill(*database, "a");
    ASSERT_TRUE(Put(*database, "b", "1").has_value());
    ASSERT_EQ(executor_.queued(), 1U);
    Gate synced;
    synced.Close([](std::string_view operation) { return operation == "sync_directory db"; });
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
    const std::size_t start = file_system_.operations().size();
    closing.Open();
    synced.Open();
    closer.join();
    runner.join();
    file_system_.SetOperationHook({});

    for (const std::string& operation : OperationsSince(start)) {
        EXPECT_FALSE(operation.starts_with("append MANIFEST-")) << operation;
    }
    ASSERT_EQ(Numbers(FileType::Table).size(), 1U);

    database = Open();
    EXPECT_EQ(Get(*database, "a"), Large());
    EXPECT_EQ(Get(*database, "b"), "1");
}

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
    const Result<std::unique_ptr<DatabaseEngine>> rejected = TryOpen();
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
    for (const std::string_view failing :
         {"open_writable ", "sync_directory ", "append MANIFEST-"}) {
        SCOPED_TRACE(failing);
        const auto database = Open();
        ASSERT_EQ(executor_.queued(), 1U);
        const std::size_t start = file_system_.operations().size();
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
        const auto state = database->GetState();
        ASSERT_TRUE(state.has_value());
        EXPECT_EQ(state->protected_output_count, 0U);
        EXPECT_FALSE(state->background_work_scheduled);
        ASSERT_TRUE(state->sticky_error.has_value());
        EXPECT_EQ(state->sticky_error->message(), "injected failure");
        for (const std::string& operation : OperationsSince(start)) {
            EXPECT_FALSE(operation.starts_with("remove ")) << operation;
            if (failing != "append MANIFEST-") {
                EXPECT_FALSE(operation.starts_with("append MANIFEST-")) << operation;
            }
        }
    }

    // Opening removes the failed outputs, and the compaction runs again.
    const auto database = Open();
    EXPECT_EQ(Numbers(FileType::Table), tables);
    ASSERT_TRUE(executor_.RunOne());
    EXPECT_EQ(Levels(), "0 1 1 0 0 0 0");
    EXPECT_EQ(Get(*database, "a"), Large());
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
    synced.Close(
        [](std::string_view operation) { return operation.starts_with("sync_directory "); });
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

}  // namespace database_test
}  // namespace modern_leveldb
