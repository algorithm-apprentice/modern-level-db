#include "engine/recovery.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "engine/table_cache.h"
#include "format/internal_key.h"
#include "format/write_batch.h"
#include "metadata/filenames.h"
#include "metadata/version.h"
#include "metadata/version_edit.h"
#include "metadata/version_set.h"
#include "modern_leveldb/base/bytes.h"
#include "modern_leveldb/base/comparator.h"
#include "modern_leveldb/base/result.h"
#include "platform/file_system.h"
#include "support/memory_file_system.h"
#include "table/table.h"
#include "wal/wal_io.h"

namespace modern_leveldb {
namespace {

using test_support::MemoryFileSystem;

class ReverseListingFileSystem final : public MemoryFileSystem {
public:
    Result<std::vector<std::filesystem::path>> ListDirectory(
        const std::filesystem::path& path) const override {
        auto names = MemoryFileSystem::ListDirectory(path);
        if (names.has_value() && reverse_listing) {
            std::ranges::reverse(*names);
        }
        return names;
    }

    bool reverse_listing = false;
};

template <typename T>
concept RecoverableWith = requires(FileSystem& file_system, T&& comparator, TableCache& cache) {
    RecoverDatabase(file_system, std::filesystem::path(), std::forward<T>(comparator),
                    RecoveryOptions{}, cache);
};
static_assert(RecoverableWith<const InternalKeyComparator&>);
static_assert(!RecoverableWith<InternalKeyComparator>);

struct Put {
    std::string_view key;
    std::string_view value;
};
struct Delete {
    std::string_view key;
};

std::vector<std::byte> Batch(SequenceNumber sequence,
                             const std::vector<std::variant<Put, Delete>>& ops) {
    EncodedWriteBatch batch;
    for (const auto& op : ops) {
        if (const auto* put = std::get_if<Put>(&op)) {
            batch.Put(AsBytes(put->key), AsBytes(put->value));
        } else {
            batch.Delete(AsBytes(std::get<Delete>(op).key));
        }
    }
    batch.SetSequence(sequence);
    const ByteView encoded = batch.encoded();
    return std::vector<std::byte>(encoded.begin(), encoded.end());
}

// Readable entries: "key@sequence" maps to the value, or to "<deleted>".
using Entries = std::map<std::string, std::string>;

class RecoveryTest : public testing::Test {
protected:
    static RecoveryOptions Creating() {
        RecoveryOptions options;
        options.create_if_missing = true;
        return options;
    }

    Result<RecoveredDatabase> TryRecover(const RecoveryOptions& options) {
        return RecoverDatabase(file_system_, directory_, comparator_, options, table_cache_);
    }

    RecoveredDatabase Recover(const RecoveryOptions& options = Creating()) {
        auto recovered = TryRecover(options);
        EXPECT_TRUE(recovered.has_value()) << recovered.error().ToString();
        return std::move(recovered).value();
    }

    void ExpectError(const RecoveryOptions& options, ErrorCode code) {
        const auto recovered = TryRecover(options);
        ASSERT_FALSE(recovered.has_value());
        EXPECT_EQ(recovered.error().code(), code) << recovered.error().ToString();
    }

    void WriteLog(std::uint64_t number, const std::vector<std::vector<std::byte>>& records) {
        auto file = file_system_.OpenWritable(LogFileName(directory_, number));
        ASSERT_TRUE(file.has_value());
        WalWriter writer(std::move(*file));
        for (const std::vector<std::byte>& record : records) {
            ASSERT_TRUE(writer.AddRecord(record).has_value());
        }
    }

    std::vector<std::byte> LogContents(std::uint64_t number) {
        return file_system_.Contents(LogFileName(directory_, number)).value();
    }

    // The operations since `start`.
    std::vector<std::string> OperationsSince(std::size_t start) const {
        const std::vector<std::string>& all = file_system_.operations();
        return std::vector<std::string>(all.begin() + static_cast<std::ptrdiff_t>(start),
                                        all.end());
    }

    // Every entry of the tables in the current version.
    static Entries TableEntries(const VersionSet& versions, TableCache& table_cache) {
        Entries entries;
        for (std::uint32_t level = 0; level < NumLevels; ++level) {
            for (const Version::File& file : versions.current()->files(level)) {
                auto table = table_cache.Find(file->number, file->file_size);
                EXPECT_TRUE(table.has_value());
                if (!table.has_value()) {
                    continue;
                }
                Table::Iterator iterator(**table);
                Status status = iterator.SeekToFirst();
                while (status.has_value() && iterator.valid()) {
                    const InternalKeyView key = ParseInternalKey(iterator.key()).value();
                    entries[std::string(AsStringView(key.user_key)) + "@" +
                            std::to_string(key.sequence)] =
                        key.kind == ValueKind::Deletion
                            ? "<deleted>"
                            : std::string(AsStringView(iterator.value()));
                    status = iterator.Next();
                }
                EXPECT_TRUE(status.has_value());
            }
        }
        return entries;
    }
    Entries TableEntries(const VersionSet& versions) {
        return TableEntries(versions, table_cache_);
    }

    // Fails every file operation of a recovery in turn and recovers again.
    void ExpectRecoveryAfterEveryFailure(const RecoveryOptions& options);

    static std::vector<std::uint64_t> TableNumbers(const VersionSet& versions,
                                                   std::uint32_t level) {
        std::vector<std::uint64_t> numbers;
        for (const Version::File& file : versions.current()->files(level)) {
            numbers.push_back(file->number);
        }
        std::ranges::sort(numbers);
        return numbers;
    }

    ReverseListingFileSystem file_system_;
    const std::filesystem::path directory_ = std::filesystem::path("db");
    InternalKeyComparator comparator_{BytewiseComparator()};
    TableCache table_cache_{file_system_, directory_, comparator_, {}, 100};
};

TEST_F(RecoveryTest, CreatesADatabaseAndItsDirectory) {
    const RecoveredDatabase recovered = Recover();

    const std::vector<std::string>& operations = file_system_.operations();
    ASSERT_GE(operations.size(), 5U);
    EXPECT_EQ(std::vector<std::string>(operations.begin(), operations.begin() + 5),
              (std::vector<std::string>{"exists db", "create_directory db", "lock LOCK",
                                        "exists CURRENT", "sync_directory ."}));
    const auto listed = std::ranges::find(operations, "list db");
    ASSERT_NE(listed, operations.end());
    EXPECT_EQ(std::vector<std::string>(listed, operations.end()),
              (std::vector<std::string>{"list db", "open_writable 000002.log", "sync 000002.log",
                                        "sync_directory db", "append MANIFEST-000001",
                                        "append MANIFEST-000001", "flush MANIFEST-000001",
                                        "sync MANIFEST-000001"}));
    ASSERT_NE(recovered.log, nullptr);
    EXPECT_EQ(recovered.log_number, 2U);
    EXPECT_EQ(recovered.versions->log_number(), 2U);
    EXPECT_EQ(recovered.versions->prev_log_number(), 0U);
    EXPECT_EQ(recovered.versions->manifest_file_number(), 1U);
    EXPECT_EQ(recovered.versions->NewFileNumber(), 3U);
    EXPECT_TRUE(TableEntries(*recovered.versions).empty());
}

TEST_F(RecoveryTest, CanSkipOnlyTheInitialWalCreationBarrier) {
    RecoveryOptions options = Creating();
    options.sync_wal_creation = false;

    const RecoveredDatabase recovered = Recover(options);

    const std::vector<std::string>& operations = file_system_.operations();
    const auto listed = std::ranges::find(operations, "list db");
    ASSERT_NE(listed, operations.end());
    EXPECT_EQ(std::vector<std::string>(listed, operations.end()),
              (std::vector<std::string>{"list db", "open_writable 000002.log",
                                        "append MANIFEST-000001", "append MANIFEST-000001",
                                        "flush MANIFEST-000001", "sync MANIFEST-000001"}));
    EXPECT_EQ(recovered.log_number, 2U);
}

TEST_F(RecoveryTest, SyncsTheParentDirectoryWhenCreatingADatabase) {
    for (const auto& [directory, parent] :
         std::vector<std::pair<std::string, std::string>>{{"root/db", "root"}, {"db/", "."}}) {
        SCOPED_TRACE(directory);
        MemoryFileSystem file_system;
        TableCache cache(file_system, directory, comparator_, {}, 10);

        auto recovered = RecoverDatabase(file_system, directory, comparator_, Creating(), cache);

        ASSERT_TRUE(recovered.has_value()) << recovered.error().ToString();
        ASSERT_GE(file_system.operations().size(), 5U);
        EXPECT_EQ(file_system.operations()[4], "sync_directory " + parent);
    }

    // A retry after a failed sync finds the directory and still syncs its parent.
    file_system_.FailOperation(4, Error::Io("injected failure"));
    ExpectError(Creating(), ErrorCode::Io);
    const std::size_t start = file_system_.operations().size();
    const RecoveredDatabase recovered = Recover();
    const std::vector<std::string> operations = OperationsSince(start);
    ASSERT_GE(operations.size(), 4U);
    EXPECT_EQ(
        std::vector<std::string>(operations.begin(), operations.begin() + 4),
        (std::vector<std::string>{"exists db", "lock LOCK", "exists CURRENT", "sync_directory ."}));
}

TEST_F(RecoveryTest, RejectsMissingAndExistingDatabasesByOption) {
    ExpectError({}, ErrorCode::InvalidArgument);
    EXPECT_EQ(file_system_.operations(), (std::vector<std::string>{"exists db"}));

    file_system_.AddDirectory(directory_);
    ExpectError({}, ErrorCode::InvalidArgument);
    EXPECT_FALSE(file_system_.Contents(directory_ / "MANIFEST-000001").has_value());

    static_cast<void>(Recover());
    RecoveryOptions exclusive = Creating();
    exclusive.error_if_exists = true;
    ExpectError(exclusive, ErrorCode::InvalidArgument);

    // Each failure released the lock.
    const RecoveredDatabase reopened = Recover({});
    EXPECT_EQ(reopened.versions->log_number(), 4U);
}

TEST_F(RecoveryTest, ReplaysLogsIntoALevel0Table) {
    static_cast<void>(Recover());
    WriteLog(2, {Batch(1, {Put{"a", "alpha"}, Put{"b", "beta"}}), Batch(3, {Delete{"a"}})});
    const auto old_log = LogContents(2);
    const Entries expected{{"a@1", "alpha"}, {"b@2", "beta"}, {"a@3", "<deleted>"}};
    const std::size_t start = file_system_.operations().size();
    {
        const RecoveredDatabase recovered = Recover({});

        const auto& tables = recovered.versions->current()->files(0);
        ASSERT_EQ(tables.size(), 1U);
        const std::size_t before_lookup = file_system_.operations().size();
        ASSERT_TRUE(
            table_cache_.Find(tables.front()->number, tables.front()->file_size).has_value());
        EXPECT_EQ(file_system_.operations().size(), before_lookup);
        EXPECT_EQ(TableEntries(*recovered.versions), expected);
        EXPECT_EQ(TableNumbers(*recovered.versions, 0), (std::vector<std::uint64_t>{4}));
        EXPECT_EQ(recovered.versions->last_sequence(), 3U);
        EXPECT_EQ(recovered.log_number, 5U);
        EXPECT_EQ(recovered.versions->log_number(), 5U);
        EXPECT_EQ(recovered.versions->prev_log_number(), 0U);
        EXPECT_EQ(file_system_.Contents(LogFileName(directory_, 2)), old_log);
    }

    // The table and the new log are durable before the new MANIFEST names them.
    const std::vector<std::string> operations = OperationsSince(start);
    const auto position = [&](std::string_view operation) {
        const auto found = std::ranges::find(operations, operation);
        EXPECT_NE(found, operations.end()) << operation;
        return found - operations.begin();
    };
    EXPECT_LT(position("close 000004.ldb"), position("open_writable 000005.log"));
    EXPECT_LT(position("open_writable 000005.log"), position("sync 000005.log"));
    EXPECT_LT(position("sync 000005.log"), position("sync_directory db"));
    EXPECT_LT(position("sync_directory db"), position("open_writable MANIFEST-000003"));

    // Reopening finds the table and replays only the new, empty log.
    const RecoveredDatabase reopened = Recover({});
    EXPECT_EQ(TableEntries(*reopened.versions), expected);
    EXPECT_EQ(TableNumbers(*reopened.versions, 0), (std::vector<std::uint64_t>{4}));
    EXPECT_EQ(reopened.versions->last_sequence(), 3U);
    EXPECT_EQ(reopened.log_number, 7U);
}

TEST_F(RecoveryTest, SplitsTablesAtTheWriteBufferSize) {
    static_cast<void>(Recover());
    // An empty batch consumes no sequence; its header names the next available one.
    WriteLog(2, {Batch(1, {Put{"a", "1"}}), Batch(2, {}), Batch(2, {Put{"b", "2"}, Put{"c", "3"}}),
                 Batch(4, {Delete{"a"}})});
    RecoveryOptions options;
    options.memtable_size_threshold = 1;

    const RecoveredDatabase recovered = Recover(options);

    EXPECT_EQ(TableNumbers(*recovered.versions, 0), (std::vector<std::uint64_t>{4, 5, 6}));
    EXPECT_EQ(TableEntries(*recovered.versions),
              (Entries{{"a@1", "1"}, {"b@2", "2"}, {"c@3", "3"}, {"a@4", "<deleted>"}}));
    EXPECT_EQ(recovered.versions->last_sequence(), 4U);
    EXPECT_EQ(recovered.log_number, 7U);
}

TEST_F(RecoveryTest, ReplaysTheLogsThatTheManifestNeeds) {
    static_cast<void>(Recover());
    {
        auto versions = VersionSet::Recover(file_system_, directory_, comparator_);
        ASSERT_TRUE(versions.has_value());
        for (int allocated = 0; allocated < 5; ++allocated) {
            static_cast<void>((*versions)->NewFileNumber());
        }
        VersionEdit edit;
        edit.SetLogNumber(7);
        edit.SetPrevLogNumber(5);
        (*versions)->SetLastSequence(100);
        ASSERT_TRUE((*versions)->LogAndApply(std::move(edit)).has_value());
    }
    WriteLog(2, {Batch(1, {Put{"old", "x"}})});
    WriteLog(4, {Batch(40, {Put{"below", "x"}})});
    WriteLog(5, {Batch(10, {Put{"previous", "p"}})});
    WriteLog(7, {Batch(20, {Put{"current", "c"}})});
    WriteLog(20, {Batch(30, {Put{"newer", "n"}})});
    file_system_.Write(directory_ / "ignored", {});
    file_system_.Write(directory_ / "000090.sst", {});
    file_system_.Write(directory_ / "000099.dbtmp", {});
    file_system_.reverse_listing = true;
    const std::size_t start = file_system_.operations().size();

    const RecoveredDatabase recovered = Recover({});

    std::vector<std::string> opened_logs;
    for (const std::string& operation : OperationsSince(start)) {
        if (operation.starts_with("open_sequential ") && operation.ends_with(".log")) {
            opened_logs.push_back(operation);
        }
    }
    EXPECT_EQ(opened_logs,
              (std::vector<std::string>{"open_sequential 000005.log", "open_sequential 000007.log",
                                        "open_sequential 000020.log"}));
    EXPECT_EQ(TableEntries(*recovered.versions),
              (Entries{{"previous@10", "p"}, {"current@20", "c"}, {"newer@30", "n"}}));
    EXPECT_EQ(TableNumbers(*recovered.versions, 0), (std::vector<std::uint64_t>{21}));
    EXPECT_EQ(recovered.versions->last_sequence(), 100U);
    EXPECT_EQ(recovered.log_number, 22U);
}

TEST_F(RecoveryTest, InstallsANewLogWithoutSelectedReplayInputs) {
    static_cast<void>(Recover());
    {
        auto versions = VersionSet::Recover(file_system_, directory_, comparator_);
        ASSERT_TRUE(versions.has_value());
        for (int allocated = 0; allocated < 5; ++allocated) {
            static_cast<void>((*versions)->NewFileNumber());
        }
        VersionEdit edit;
        edit.SetLogNumber(7);
        edit.SetPrevLogNumber(0);
        (*versions)->SetLastSequence(100);
        ASSERT_TRUE((*versions)->LogAndApply(std::move(edit)).has_value());
    }
    const std::size_t start = file_system_.operations().size();
    const auto recovered = TryRecover({});
    ASSERT_TRUE(recovered.has_value());
    EXPECT_TRUE(TableNumbers(*recovered->versions, 0).empty());
    EXPECT_EQ(recovered->versions->last_sequence(), 100U);
    EXPECT_EQ(recovered->versions->prev_log_number(), 0U);
    EXPECT_EQ(recovered->log_number, 10U);
    EXPECT_EQ(recovered->versions->log_number(), recovered->log_number);
    const auto contents = file_system_.Contents(LogFileName(directory_, recovered->log_number));
    ASSERT_TRUE(contents.has_value());
    EXPECT_TRUE(contents->empty());
    const auto operations = OperationsSince(start);
    EXPECT_TRUE(std::ranges::none_of(operations, [](std::string_view operation) {
        return operation.starts_with("open_sequential ") && operation.ends_with(".log");
    }));
    EXPECT_NE(std::ranges::find(operations, "sync 000010.log"), operations.end());
    EXPECT_NE(std::ranges::find(operations, "append MANIFEST-000009"), operations.end());
}

TEST_F(RecoveryTest, StopsBeforeLaterReplayAndInstallationAfterAReplayFailure) {
    static_cast<void>(Recover());
    WriteLog(2, {Batch(1, {Put{"a", "1"}})});
    WriteLog(3, {Batch(2, {Put{"b", "2"}})});
    file_system_.SetOperationHook([](std::string_view operation) -> Status {
        if (operation == "open_sequential 000002.log") {
            return std::unexpected(Error::Io("replay open failed"));
        }
        return {};
    });
    const std::size_t start = file_system_.operations().size();
    const auto failed = TryRecover({});
    ASSERT_FALSE(failed.has_value());
    EXPECT_EQ(failed.error().code(), ErrorCode::Io);
    EXPECT_EQ(failed.error().message(), "replay open failed");
    const auto operations = OperationsSince(start);
    EXPECT_EQ(std::ranges::find(operations, "open_sequential 000003.log"), operations.end());
    EXPECT_TRUE(std::ranges::none_of(operations, [](std::string_view operation) {
        return operation.starts_with("open_writable ");
    }));
    file_system_.SetOperationHook({});
    const auto recovered = TryRecover({});
    ASSERT_TRUE(recovered.has_value());
    EXPECT_EQ(TableEntries(*recovered->versions), (Entries{{"a@1", "1"}, {"b@2", "2"}}));
}

TEST_F(RecoveryTest, SkipsDamagedRecordsAndTruncatedTails) {
    static_cast<void>(Recover());
    // The large record spans two log blocks, so the record after it starts the
    // second block, after the damage.
    const std::vector<std::byte> first = Batch(1, {Put{"a", "1"}});
    const std::string large(40000, 'x');
    WriteLog(2, {first, Batch(2, {Put{"large", large}}), Batch(3, {Put{"c", "3"}})});
    std::vector<std::byte> damaged = LogContents(2);
    damaged[2 * WalHeaderSize + first.size() + 5] ^= std::byte{0x01};
    file_system_.Write(LogFileName(directory_, 2), damaged);
    WriteLog(3, {Batch(4, {Put{"e", "4"}}), Batch(5, {Put{"f", "5"}})});
    std::vector<std::byte> truncated = LogContents(3);
    truncated.resize(truncated.size() - 2);
    file_system_.Write(LogFileName(directory_, 3), truncated);
    // An empty batch shows that the sequences before its own were used, even if
    // damage dropped the records that used them.
    WriteLog(4, {Batch(6, {Put{"g", "6"}}), Batch(10, {})});

    const RecoveredDatabase recovered = Recover({});

    EXPECT_EQ(TableEntries(*recovered.versions),
              (Entries{{"a@1", "1"}, {"c@3", "3"}, {"e@4", "4"}, {"g@6", "6"}}));
    EXPECT_EQ(recovered.versions->last_sequence(), 9U);
}

TEST_F(RecoveryTest, RejectsRecordsThatNoWriterProduces) {
    static_cast<void>(Recover());
    RecoveryOptions splitting;
    splitting.memtable_size_threshold = 0;
    const std::vector<std::pair<std::vector<std::vector<std::byte>>, RecoveryOptions>> cases{
        {{{std::byte{1}, std::byte{2}, std::byte{3}}}, {}},
        {{Batch(0, {Put{"a", "1"}})}, {}},
        {{Batch(5, {Put{"a", "1"}}), Batch(5, {Put{"b", "2"}})}, {}},
        {{Batch(5, {Put{"a", "1"}, Put{"b", "2"}}), Batch(6, {Put{"c", "3"}})}, {}},
        {{Batch(5, {Put{"a", "1"}}), Batch(5, {})}, {}},
        {{Batch(5, {}), Batch(4, {Put{"a", "1"}})}, {}},
        // A duplicate entry in a later memtable than the first.
        {{Batch(5, {Put{"a", "1"}}), Batch(5, {Put{"a", "1"}})}, splitting},
    };
    for (std::size_t index = 0; index < cases.size(); ++index) {
        SCOPED_TRACE(index);
        WriteLog(2, cases[index].first);
        ExpectError(cases[index].second, ErrorCode::Corruption);
    }

    // A failed recovery leaves no table cached, so the table that replaces the
    // one the last case wrote is read from its file.
    WriteLog(2, {Batch(5, {Put{"a", "new"}}), Batch(6, {Put{"a", "2"}})});
    const RecoveredDatabase recovered = Recover(splitting);
    EXPECT_EQ(TableEntries(*recovered.versions), (Entries{{"a@5", "new"}, {"a@6", "2"}}));
}

TEST_F(RecoveryTest, RejectsMissingTables) {
    static_cast<void>(Recover());
    WriteLog(2, {Batch(1, {Put{"a", "1"}})});
    static_cast<void>(Recover({}));
    const std::filesystem::path table = TableFileName(directory_, 4);
    const std::vector<std::byte> contents = file_system_.Contents(table).value();
    file_system_.Erase(table);
    // Neither a log with the table's number nor a legacy table name counts.
    file_system_.Write(LogFileName(directory_, 4), {});
    file_system_.Write(directory_ / "000004.sst", contents);

    ExpectError({}, ErrorCode::Corruption);

    file_system_.Write(table, contents);
    EXPECT_EQ(TableEntries(*Recover({}).versions), (Entries{{"a@1", "1"}}));
}

TEST_F(RecoveryTest, RejectsLogNumbersBeyondTheLimit) {
    static_cast<void>(Recover());
    file_system_.Write(LogFileName(directory_, FileNumberLimit), {});

    ExpectError({}, ErrorCode::Corruption);
}

TEST_F(RecoveryTest, ValidatesReferencedTablesBeforeReplayLogNumbers) {
    static_cast<void>(Recover());
    WriteLog(2, {Batch(1, {Put{"a", "1"}})});
    static_cast<void>(Recover({}));
    const auto table = TableFileName(directory_, 4);
    const auto contents = file_system_.Contents(table).value();
    file_system_.Erase(table);
    const auto invalid_log = LogFileName(directory_, FileNumberLimit);
    file_system_.Write(invalid_log, {});

    const auto missing_table = TryRecover({});
    ASSERT_FALSE(missing_table.has_value());
    EXPECT_EQ(missing_table.error().code(), ErrorCode::Corruption);
    EXPECT_EQ(missing_table.error().message(), "table file is missing: db/000004.ldb");

    file_system_.Write(table, contents);
    const auto invalid_number = TryRecover({});
    ASSERT_FALSE(invalid_number.has_value());
    EXPECT_EQ(invalid_number.error().code(), ErrorCode::Corruption);
    EXPECT_EQ(
        invalid_number.error().message(),
        "log file number is beyond the file number limit: " + std::to_string(FileNumberLimit));

    file_system_.Erase(invalid_log);
    EXPECT_EQ(TableEntries(*Recover({}).versions), (Entries{{"a@1", "1"}}));
}

TEST_F(RecoveryTest, RejectsASecondRecoveryWhileLocked) {
    const RecoveredDatabase recovered = Recover();

    ExpectError({}, ErrorCode::Busy);
}

TEST_F(RecoveryTest, CreatesTheDatabaseAfterAFailure) {
    MemoryFileSystem reference;
    {
        TableCache cache(reference, directory_, comparator_, {}, 10);
        ASSERT_TRUE(RecoverDatabase(reference, directory_, comparator_, Creating(), cache));
    }
    const std::size_t count = reference.operations().size();

    for (std::size_t failing = 0; failing < count; ++failing) {
        SCOPED_TRACE(reference.operations()[failing]);
        MemoryFileSystem file_system;
        TableCache cache(file_system, directory_, comparator_, {}, 10);
        file_system.FailOperation(failing, Error::Io("injected failure"));

        const auto failed =
            RecoverDatabase(file_system, directory_, comparator_, Creating(), cache);

        ASSERT_FALSE(failed.has_value());
        EXPECT_EQ(failed.error().code(), ErrorCode::Io);
        EXPECT_EQ(failed.error().message(), "injected failure");
        auto recovered = RecoverDatabase(file_system, directory_, comparator_, Creating(), cache);
        ASSERT_TRUE(recovered.has_value()) << recovered.error().ToString();
        EXPECT_TRUE(TableEntries(*recovered->versions, cache).empty());
    }
}

TEST_F(RecoveryTest, RecoversEveryEntryAfterAFailure) {
    // With a one-byte write buffer, every record is written to its own table
    // while the logs are replayed; by default, the last memtable holds them all.
    RecoveryOptions splitting;
    splitting.memtable_size_threshold = 1;
    for (const RecoveryOptions& options : {splitting, RecoveryOptions{}}) {
        SCOPED_TRACE(options.memtable_size_threshold);
        ExpectRecoveryAfterEveryFailure(options);
    }
}

TEST_F(RecoveryTest, EvictsBuiltTablesAndReleasesTheLockAfterAnException) {
    const std::vector<std::pair<std::string_view, std::size_t>> failure_points{
        {"open_sequential 000003.log", 1},
        {"sync MANIFEST-000003", 2},
    };
    for (const auto& [throwing_operation, expected_tables] : failure_points) {
        SCOPED_TRACE(throwing_operation);
        MemoryFileSystem file_system;
        TableCache cache(file_system, directory_, comparator_, {}, 10);
        {
            const auto created =
                RecoverDatabase(file_system, directory_, comparator_, Creating(), cache);
            ASSERT_TRUE(created.has_value());
        }
        const std::vector<std::pair<std::uint64_t, std::vector<std::byte>>> records{
            {2, Batch(1, {Put{"a", "1"}})},
            {3, Batch(2, {Put{"b", "2"}})},
        };
        for (const auto& [number, record] : records) {
            auto file = file_system.OpenWritable(LogFileName(directory_, number));
            ASSERT_TRUE(file.has_value());
            WalWriter writer(std::move(*file));
            ASSERT_TRUE(writer.AddRecord(record).has_value());
        }
        RecoveryOptions options;
        options.memtable_size_threshold = 1;
        bool injected = false;
        file_system.SetOperationHook([&](std::string_view operation) -> Status {
            if (operation == throwing_operation) {
                injected = true;
                throw std::runtime_error("injected recovery exception");
            }
            return {};
        });

        EXPECT_THROW(static_cast<void>(
                         RecoverDatabase(file_system, directory_, comparator_, options, cache)),
                     std::runtime_error);
        file_system.SetOperationHook({});
        ASSERT_TRUE(injected);

        const auto names = file_system.ListDirectory(directory_);
        ASSERT_TRUE(names.has_value());
        std::size_t table_count = 0;
        for (const auto& name : *names) {
            const auto parsed = ParseFileName(name.string());
            if (!parsed.has_value() || parsed->type != FileType::Table) {
                continue;
            }
            ++table_count;
            const auto contents = file_system.Contents(directory_ / name);
            ASSERT_TRUE(contents.has_value());
            const std::size_t before_lookup = file_system.operations().size();
            ASSERT_TRUE(cache.Find(parsed->number, contents->size()).has_value());
            ASSERT_GT(file_system.operations().size(), before_lookup);
            EXPECT_EQ(file_system.operations()[before_lookup],
                      "open_random_access " + name.string());
            cache.Evict(parsed->number);
        }
        EXPECT_EQ(table_count, expected_tables);
        const auto recovered =
            RecoverDatabase(file_system, directory_, comparator_, options, cache);
        ASSERT_TRUE(recovered.has_value()) << recovered.error().ToString();
        EXPECT_EQ(TableEntries(*recovered->versions, cache), (Entries{{"a@1", "1"}, {"b@2", "2"}}));
    }
}

void RecoveryTest::ExpectRecoveryAfterEveryFailure(const RecoveryOptions& options) {
    const auto populate = [&](MemoryFileSystem& file_system) {
        TableCache cache(file_system, directory_, comparator_, {}, 10);
        ASSERT_TRUE(RecoverDatabase(file_system, directory_, comparator_, Creating(), cache));
        for (const auto& [number, records] :
             std::vector<std::pair<std::uint64_t, std::vector<std::vector<std::byte>>>>{
                 {2, {Batch(1, {Put{"a", "1"}}), Batch(2, {Put{"b", "2"}})}},
                 {3, {Batch(3, {Put{"c", "3"}, Delete{"a"}})}}}) {
            auto file = file_system.OpenWritable(LogFileName(directory_, number));
            ASSERT_TRUE(file.has_value());
            WalWriter writer(std::move(*file));
            for (const std::vector<std::byte>& record : records) {
                ASSERT_TRUE(writer.AddRecord(record).has_value());
            }
        }
    };
    const Entries expected{{"a@1", "1"}, {"b@2", "2"}, {"c@3", "3"}, {"a@4", "<deleted>"}};

    MemoryFileSystem reference;
    populate(reference);
    const std::size_t reference_start = reference.operations().size();
    std::size_t count = 0;
    {
        TableCache cache(reference, directory_, comparator_, {}, 10);
        auto recovered = RecoverDatabase(reference, directory_, comparator_, options, cache);
        ASSERT_TRUE(recovered.has_value());
        count = reference.operations().size() - reference_start;
        EXPECT_EQ(TableEntries(*recovered->versions, cache), expected);
    }

    for (std::size_t failing = 0; failing < count; ++failing) {
        SCOPED_TRACE(reference.operations()[reference_start + failing]);
        MemoryFileSystem file_system;
        populate(file_system);
        TableCache cache(file_system, directory_, comparator_, {}, 10);
        const std::size_t start = file_system.operations().size();
        file_system.FailOperation(start + failing, Error::Io("injected failure"));

        const auto failed = RecoverDatabase(file_system, directory_, comparator_, options, cache);

        ASSERT_FALSE(failed.has_value());
        EXPECT_EQ(failed.error().code(), ErrorCode::Io);
        EXPECT_EQ(failed.error().message(), "injected failure");
        // No table that the failed recovery wrote is cached.
        const std::vector<std::string> operations(
            file_system.operations().begin() + static_cast<std::ptrdiff_t>(start),
            file_system.operations().end());
        for (const std::string& operation : operations) {
            constexpr std::string_view Opened = "open_writable ";
            if (!operation.starts_with(Opened)) {
                continue;
            }
            const std::string name = operation.substr(Opened.size());
            const std::optional<ParsedFileName> parsed = ParseFileName(name);
            const auto contents = file_system.Contents(directory_ / name);
            if (!parsed.has_value() || parsed->type != FileType::Table || !contents.has_value()) {
                continue;
            }
            const std::size_t before = file_system.operations().size();
            ASSERT_TRUE(cache.Find(parsed->number, contents->size()).has_value());
            EXPECT_EQ(file_system.operations()[before], "open_random_access " + name);
            cache.Evict(parsed->number);
        }

        // The lock was released, and recovering again with the same table cache
        // recovers every entry.
        auto recovered = RecoverDatabase(file_system, directory_, comparator_, options, cache);
        ASSERT_TRUE(recovered.has_value()) << recovered.error().ToString();
        EXPECT_EQ(TableEntries(*recovered->versions, cache), expected);
    }
}

}  // namespace
}  // namespace modern_leveldb
