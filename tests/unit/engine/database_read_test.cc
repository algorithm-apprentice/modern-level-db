#include "database_test_support.h"

namespace modern_leveldb {
namespace database_test {

class DatabaseSeekTest : public DatabaseTest {
protected:
    void FillLevels1And2(DatabaseEngine& database) {
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
    static void ReadPastTheLevel1Table(DatabaseEngine& database, int times) {
        for (int i = 0; i < times; ++i) {
            ASSERT_EQ(Get(database, "m"), "<none>");
        }
    }
};

TEST_F(DatabaseTest, ReadPinsSurviveMemtableFlushAndVersionInstall) {
    auto database = Open();
    ASSERT_TRUE(Put(*database, "a", "1").has_value());
    ASSERT_TRUE(Flush(*database).has_value());
    const std::uint64_t table = Numbers(FileType::Table).front();
    database.reset();
    database = Open();

    Gate read;
    const std::string blocked_read = "read " + Numbered(table, ".ldb");
    read.Close([&](std::string_view operation) { return operation == blocked_read; });
    file_system_.SetOperationHook(std::ref(read));
    std::future<std::string> value =
        std::async(std::launch::async, [&] { return Get(*database, "a"); });
    read.WaitUntilReached();

    Fill(*database, "b");
    ASSERT_TRUE(Flush(*database).has_value());
    read.Open();
    ASSERT_EQ(value.wait_for(std::chrono::seconds(10)), std::future_status::ready);
    const std::string result = value.get();
    file_system_.SetOperationHook({});

    EXPECT_EQ(result, "1");
}

TEST_F(DatabaseTest, ReadPinKeepsAnExistingImmutableAliveWhileItsFlushCompletes) {
    BlockingComparator comparator;
    options_.comparator = &comparator;
    auto database = Open();
    Fill(*database, "immutable");
    ASSERT_TRUE(Put(*database, "current", "1").has_value());
    ASSERT_EQ(executor_.queued(), 1U);

    comparator.Close("immutable");
    std::future<std::string> value =
        std::async(std::launch::async, [&] { return Get(*database, "immutable"); });
    comparator.WaitUntilReached();

    const bool flushed = executor_.RunOne();
    comparator.Open();
    ASSERT_TRUE(flushed);
    ASSERT_EQ(value.wait_for(std::chrono::seconds(10)), std::future_status::ready);
    EXPECT_EQ(value.get(), Large());
}

TEST_F(DatabaseTest, ReadPinsReleaseWhenATableReadThrows) {
    auto database = Open();
    ASSERT_TRUE(Put(*database, "a", "1").has_value());
    ASSERT_TRUE(Flush(*database).has_value());
    database.reset();
    database = Open();

    file_system_.SetOperationHook([](std::string_view operation) -> Status {
        if (operation.starts_with("read ") && operation.ends_with(".ldb")) {
            throw std::runtime_error("thrown");
        }
        return {};
    });
    std::vector<std::byte> value;
    EXPECT_THROW(static_cast<void>(database->Get(AsBytes("a"), value)), std::runtime_error);
    file_system_.SetOperationHook({});

    EXPECT_EQ(Get(*database, "a"), "1");
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

    DatabaseEngineReadOptions uncached;
    uncached.fill_cache = false;
    std::vector<std::byte> value;
    ASSERT_TRUE(database->Get(AsBytes("a"), value, uncached).has_value());
    const std::unique_ptr<DbIterator> iterator = database->NewIterator(uncached);
    ASSERT_TRUE(iterator->SeekToFirst().has_value());
    EXPECT_EQ(blocks.total_charge(), 0U);
    EXPECT_EQ(Get(*database, "a"), "1");
    EXPECT_GT(blocks.total_charge(), 0U);
}

TEST_F(DatabaseTest, ReportsPublishedStateAndExplicitSnapshots) {
    const auto database = Open();
    Result<DatabaseEngineState> initial = database->GetState();
    ASSERT_TRUE(initial.has_value()) << initial.error().ToString();
    EXPECT_EQ(initial->last_sequence, 0U);
    EXPECT_EQ(initial->snapshot_count, 0U);
    EXPECT_FALSE(initial->oldest_snapshot_sequence.has_value());
    EXPECT_EQ(initial->write_queue_depth, 0U);
    EXPECT_GT(initial->mutable_memtable_bytes, 0U);
    EXPECT_FALSE(initial->immutable_memtable_bytes.has_value());
    EXPECT_EQ(initial->protected_output_count, 0U);
    EXPECT_FALSE(initial->background_work_scheduled);
    EXPECT_FALSE(initial->sticky_error.has_value());
    for (const DatabaseEngineLevelState& level : initial->levels) {
        EXPECT_EQ(level.file_count, 0U);
        EXPECT_EQ(level.file_bytes, 0U);
    }

    ASSERT_TRUE(Put(*database, "small", "1").has_value());
    Result<DatabaseEngineState> small = database->GetState();
    ASSERT_TRUE(small.has_value()) << small.error().ToString();
    EXPECT_EQ(small->last_sequence, 1U);
    EXPECT_EQ(small->mutable_memtable_bytes, initial->mutable_memtable_bytes);

    EncodedWriteBatch empty;
    ASSERT_TRUE(database->Write(empty, false).has_value());
    Result<DatabaseEngineState> empty_written = database->GetState();
    ASSERT_TRUE(empty_written.has_value()) << empty_written.error().ToString();
    EXPECT_EQ(empty_written->last_sequence, small->last_sequence);
    EXPECT_EQ(empty_written->mutable_memtable_bytes, small->mutable_memtable_bytes);

    const SequenceNumber oldest = database->GetSnapshot();
    const std::string large(5000, 'v');
    ASSERT_TRUE(Put(*database, "large", large).has_value());
    const SequenceNumber newest = database->GetSnapshot();
    const SequenceNumber duplicate = database->GetSnapshot();
    Result<DatabaseEngineState> snapshotted = database->GetState();
    ASSERT_TRUE(snapshotted.has_value()) << snapshotted.error().ToString();
    EXPECT_EQ(snapshotted->last_sequence, 2U);
    EXPECT_GT(snapshotted->mutable_memtable_bytes, small->mutable_memtable_bytes);
    EXPECT_EQ(snapshotted->snapshot_count, 3U);
    EXPECT_EQ(snapshotted->oldest_snapshot_sequence, oldest);

    database->ReleaseSnapshot(oldest);
    database->ReleaseSnapshot(newest);
    database->ReleaseSnapshot(duplicate);
    Result<DatabaseEngineState> released = database->GetState();
    ASSERT_TRUE(released.has_value()) << released.error().ToString();
    EXPECT_EQ(released->snapshot_count, 0U);
    EXPECT_FALSE(released->oldest_snapshot_sequence.has_value());
}

TEST_F(DatabaseTest, StateKeepsThePublishedPairWhileACommitInsertsUnlocked) {
    BlockingComparator comparator;
    options_.comparator = &comparator;
    const auto database = Open();
    ASSERT_TRUE(Put(*database, "a", "1").has_value());
    Result<DatabaseEngineState> before = database->GetState();
    ASSERT_TRUE(before.has_value()) << before.error().ToString();

    const std::string large = Large();
    comparator.Close("b");
    std::optional<Status> written;
    std::thread writer([&] { written = Put(*database, "b", large); });
    comparator.WaitUntilReached();

    Result<DatabaseEngineState> inserting = database->GetState();
    ASSERT_TRUE(inserting.has_value()) << inserting.error().ToString();
    EXPECT_EQ(inserting->last_sequence, before->last_sequence);
    EXPECT_EQ(inserting->mutable_memtable_bytes, before->mutable_memtable_bytes);
    EXPECT_EQ(inserting->write_queue_depth, 1U);
    EXPECT_FALSE(inserting->sticky_error.has_value());

    comparator.Open();
    writer.join();
    ASSERT_TRUE(written->has_value());
    Result<DatabaseEngineState> after = database->GetState();
    ASSERT_TRUE(after.has_value()) << after.error().ToString();
    EXPECT_EQ(after->last_sequence, before->last_sequence + 1U);
    EXPECT_GT(after->mutable_memtable_bytes, before->mutable_memtable_bytes);
    EXPECT_EQ(after->write_queue_depth, 0U);
}

TEST_F(DatabaseTest, ReportsImmutableAndProtectedFlushState) {
    const auto database = Open();
    ASSERT_TRUE(Put(*database, "a", "1").has_value());
    std::optional<Status> flushed;
    std::thread flusher([&] { flushed = database->FlushMemTable(); });
    executor_.WaitForTask();

    Result<DatabaseEngineState> rotated = database->GetState();
    ASSERT_TRUE(rotated.has_value()) << rotated.error().ToString();
    EXPECT_TRUE(rotated->immutable_memtable_bytes.has_value());
    EXPECT_TRUE(rotated->background_work_scheduled);
    EXPECT_EQ(rotated->protected_output_count, 0U);

    Gate output;
    output.Close(OpensATable);
    file_system_.SetOperationHook(std::ref(output));
    std::thread runner([&] { executor_.RunAll(); });
    output.WaitUntilReached();

    Result<DatabaseEngineState> building = database->GetState();
    ASSERT_TRUE(building.has_value()) << building.error().ToString();
    EXPECT_TRUE(building->immutable_memtable_bytes.has_value());
    EXPECT_TRUE(building->background_work_scheduled);
    EXPECT_EQ(building->protected_output_count, 1U);

    output.Open();
    runner.join();
    flusher.join();
    file_system_.SetOperationHook({});
    ASSERT_TRUE(flushed->has_value());

    Result<DatabaseEngineState> installed = database->GetState();
    ASSERT_TRUE(installed.has_value()) << installed.error().ToString();
    EXPECT_FALSE(installed->immutable_memtable_bytes.has_value());
    EXPECT_FALSE(installed->background_work_scheduled);
    EXPECT_EQ(installed->protected_output_count, 0U);
    std::size_t files = 0;
    for (const DatabaseEngineLevelState& level : installed->levels) {
        files += level.file_count;
    }
    EXPECT_EQ(files, 1U);
}

TEST_F(DatabaseTest, ReportsProtectedOutputAndStickyErrorAfterBackgroundException) {
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
    Result<DatabaseEngineState> state = database->GetState();
    ASSERT_TRUE(state.has_value()) << state.error().ToString();
    EXPECT_EQ(state->protected_output_count, 1U);
    EXPECT_FALSE(state->background_work_scheduled);
    ASSERT_TRUE(state->sticky_error.has_value());
    EXPECT_EQ(state->sticky_error->code(), ErrorCode::Aborted);
}

TEST_F(DatabaseTest, RejectsUnrepresentableLevelByteTotals) {
    {
        const auto database = Open();
    }
    const InternalKeyComparator comparator(BytewiseComparator());
    auto versions = VersionSet::Recover(file_system_, directory_, comparator);
    ASSERT_TRUE(versions.has_value());
    VersionEdit edit;
    for (const std::string_view key : {"a", "b"}) {
        const std::uint64_t number = (*versions)->NewFileNumber();
        Result<InternalKey> smallest = InternalKey::Create(AsBytes(key), 1, ValueKind::Value);
        Result<InternalKey> largest = InternalKey::Create(AsBytes(key), 1, ValueKind::Value);
        ASSERT_TRUE(smallest.has_value() && largest.has_value());
        FileMetadata file{.number = number,
                          .file_size = std::numeric_limits<std::uint64_t>::max(),
                          .smallest = std::move(*smallest),
                          .largest = std::move(*largest)};
        ASSERT_TRUE(edit.AddFile(0, std::move(file)).has_value());
        file_system_.Write(TableFileName(directory_, number), {});
    }
    ASSERT_TRUE((*versions)->LogAndApply(std::move(edit)).has_value());
    versions->reset();

    const auto database = Open();
    const Result<DatabaseEngineState> state = database->GetState();
    ASSERT_FALSE(state.has_value());
    EXPECT_EQ(state.error().code(), ErrorCode::Aborted);
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

    std::vector<std::byte> output;
    const auto value = database->Get(AsBytes("a"), output);
    file_system_.SetOperationHook({});

    ASSERT_FALSE(value.has_value());
    EXPECT_EQ(value.error().message(), "injected failure");
    EXPECT_EQ(Get(*database, "a"), "1");
}

// Flushes two tables from "a" to "z", which land in levels 2 and 1.

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

TEST_F(DatabaseSeekTest, ChargesTheFirstFileWhenTheSecondFileReadFails) {
    const auto database = Open();
    FillLevels1And2(*database);
    DatabaseEngineReadOptions uncached;
    uncached.fill_cache = false;
    std::vector<std::byte> output;
    for (int i = 0; i < 99; ++i) {
        const Result<bool> missing = database->Get(AsBytes("m"), output, uncached);
        ASSERT_TRUE(missing.has_value());
        EXPECT_FALSE(*missing);
    }
    ASSERT_EQ(executor_.queued(), 0U);

    const std::vector<std::vector<std::uint64_t>> files = Files();
    ASSERT_EQ(files[2].size(), 1U);
    const std::string second_read = "read " + Numbered(files[2].front(), ".ldb");
    file_system_.SetOperationHook([&](std::string_view operation) -> Status {
        if (operation == second_read) {
            return std::unexpected(Error::Io("second file failed"));
        }
        return {};
    });
    const Result<bool> failed = database->Get(AsBytes("m"), output, uncached);
    file_system_.SetOperationHook({});

    ASSERT_FALSE(failed.has_value());
    EXPECT_EQ(failed.error().message(), "second file failed");
    ASSERT_EQ(executor_.queued(), 1U);
    ASSERT_TRUE(executor_.RunOne());
    EXPECT_EQ(Levels(), "0 0 1 0 0 0 0");
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

}  // namespace database_test
}  // namespace modern_leveldb
