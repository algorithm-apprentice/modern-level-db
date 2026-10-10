#include "engine/recovery.h"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "engine/build_table.h"
#include "engine/write_path.h"
#include "format/write_batch.h"
#include "memory/memtable.h"
#include "metadata/filenames.h"
#include "metadata/version.h"
#include "metadata/version_edit.h"
#include "modern_leveldb/base/bytes.h"
#include "platform/path.h"

namespace modern_leveldb {
namespace {

// Checks a step that the recovery's invariants make valid.
void Expect(const Status& status) noexcept {
    assert(status.has_value());
    static_cast<void>(status);
}

// GCOVR_EXCL_START: only keys larger than 4 GiB reach this function
std::unexpected<Error> InvalidEntry(const Error& error) {
    return std::unexpected(
        Error::Corruption("log record's entry is not valid: " + std::string(error.message())));
}
// GCOVR_EXCL_STOP

// Returns the directory that holds the named directory's entry.
std::filesystem::path ParentDirectory(std::filesystem::path directory) {
    if (!directory.has_filename()) {
        directory = directory.parent_path();
    }
    std::filesystem::path parent = directory.parent_path();
    if (parent.empty()) {
        parent = ".";
    }
    return parent;
}

// Creates the database directory if it is missing and allowed.
Status PrepareDirectory(FileSystem& file_system, const std::filesystem::path& directory,
                        const RecoveryOptions& options) {
    const Result<bool> exists = file_system.FileExists(directory);
    if (!exists.has_value()) {
        return std::unexpected(exists.error());
    }
    if (*exists) {
        return {};
    }
    if (!options.create_if_missing) {
        return std::unexpected(
            Error::InvalidArgument("database directory does not exist: " +
                                   PathUtf8(directory).value_or("<unrepresentable>")));
    }
    return file_system.CreateDirectory(directory);
}

Result<std::unique_ptr<VersionSet>> OpenVersions(FileSystem& file_system,
                                                 const std::filesystem::path& directory,
                                                 const InternalKeyComparator& comparator,
                                                 const RecoveryOptions& options) {
    const Result<bool> exists = file_system.FileExists(CurrentFileName(directory));
    if (!exists.has_value()) {
        return std::unexpected(exists.error());
    }
    if (!*exists) {
        if (!options.create_if_missing) {
            return std::unexpected(Error::InvalidArgument(
                "database does not exist: " + PathUtf8(directory).value_or("<unrepresentable>")));
        }
        // The directory's entry must be durable, even if an earlier attempt
        // created the directory.
        const Status synced = file_system.SyncDirectory(ParentDirectory(directory));
        if (!synced.has_value()) {
            return std::unexpected(synced.error());
        }
        return VersionSet::Create(file_system, directory, comparator);
    }
    if (options.error_if_exists) {
        return std::unexpected(Error::InvalidArgument(
            "database exists: " + PathUtf8(directory).value_or("<unrepresentable>")));
    }
    return VersionSet::Recover(file_system, directory, comparator);
}

Status ValidateReferencedTableFiles(const Version& version,
                                    const std::set<std::uint64_t>& existing_table_numbers,
                                    const std::filesystem::path& directory) {
    for (std::uint32_t level = 0; level < NumLevels; ++level) {
        for (const Version::File& file : version.files(level)) {
            if (!existing_table_numbers.contains(file->number)) {
                return std::unexpected(Error::Corruption(
                    "table file is missing: " + PathUtf8(TableFileName(directory, file->number))
                                                    .value_or("<unrepresentable>")));
            }
        }
    }
    return {};
}

Status MarkReplayLogNumbersUsed(VersionSet& versions, std::span<const std::uint64_t> log_numbers) {
    for (const std::uint64_t number : log_numbers) {
        if (!versions.MarkFileNumberUsed(number).has_value()) {
            return std::unexpected(Error::Corruption(
                "log file number is beyond the file number limit: " + std::to_string(number)));
        }
    }
    return {};
}

// Validates referenced tables and reserves the selected log numbers before replay.
Result<std::vector<std::uint64_t>> PrepareLogsForReplay(FileSystem& file_system,
                                                        const std::filesystem::path& directory,
                                                        VersionSet& versions) {
    const Result<std::vector<std::filesystem::path>> names = file_system.ListDirectory(directory);
    if (!names.has_value()) {
        return std::unexpected(names.error());
    }
    std::set<std::uint64_t> tables;
    std::vector<std::uint64_t> logs;
    for (const std::filesystem::path& name : *names) {
        const std::optional<ParsedFileName> parsed = ParseNativeFileName(name);
        if (!parsed.has_value()) {
            continue;
        }
        if (parsed->type == FileType::Table) {
            tables.insert(parsed->number);
        } else if (parsed->type == FileType::Log &&
                   (parsed->number >= versions.log_number() ||
                    parsed->number == versions.prev_log_number())) {
            logs.push_back(parsed->number);
        }
    }

    const Status validated = ValidateReferencedTableFiles(*versions.current(), tables, directory);
    if (!validated.has_value()) {
        return std::unexpected(validated.error());
    }

    // Replay oldest logs first so later sequences cannot be applied ahead of
    // their history. Reserve all discovered numbers before creating recovery outputs.
    std::ranges::sort(logs);
    const Status marked = MarkReplayLogNumbersUsed(versions, logs);
    if (!marked.has_value()) {
        return std::unexpected(marked.error());
    }
    return logs;
}

// Evicts the tables that a recovery wrote from the table cache unless the
// recovery succeeds.
class BuiltTables final {
public:
    explicit BuiltTables(TableCache& table_cache) noexcept : table_cache_(&table_cache) {}
    BuiltTables(const BuiltTables&) = delete;
    BuiltTables& operator=(const BuiltTables&) = delete;
    BuiltTables(BuiltTables&&) = delete;
    BuiltTables& operator=(BuiltTables&&) = delete;
    ~BuiltTables() {
        for (const std::uint64_t number : numbers_) {
            table_cache_->Evict(number);
        }
    }

    void Add(std::uint64_t number) { numbers_.push_back(number); }
    void Keep() noexcept { numbers_.clear(); }

private:
    TableCache* table_cache_;
    std::vector<std::uint64_t> numbers_;
};

// Replays logs into memtables and writes them to level-0 tables in the edit.
class LogReplay final {
public:
    LogReplay(FileSystem& file_system, const std::filesystem::path& directory,
              const InternalKeyComparator& comparator, const RecoveryOptions& options,
              TableCache& table_cache, VersionSet& versions, VersionEdit& edit, BuiltTables& built)
        : file_system_(&file_system),
          directory_(&directory),
          comparator_(&comparator),
          options_(&options),
          table_cache_(&table_cache),
          versions_(&versions),
          edit_(&edit),
          built_(&built),
          memtable_(std::make_unique<MemTable>(comparator.user_comparator())) {}

    // Replays complete logical batches. Physical corruption can be skipped here,
    // unlike authoritative MANIFEST edits; intact malformed batches still fail recovery.
    [[nodiscard]] Status ReplayLog(std::uint64_t number) {
        Result<std::unique_ptr<SequentialFile>> file =
            file_system_->OpenSequential(LogFileName(*directory_, number));
        if (!file.has_value()) {
            return std::unexpected(std::move(file).error());
        }
        WalReader reader(std::move(*file));
        while (true) {
            const WalReadResult event = reader.ReadNext();
            if (!event.has_value()) {
                return std::unexpected(event.error());
            }
            if (!event->has_value()) {
                return {};
            }
            const auto* record = std::get_if<WalLogicalRecord>(&**event);
            if (record == nullptr) {
                continue;
            }
            const Status applied = Apply(record->data);
            if (!applied.has_value()) {
                return applied;
            }
        }
    }

    // Writes the last memtable and returns the last sequence that the replayed
    // batches used, or zero if there were none.
    [[nodiscard]] Result<SequenceNumber> Finish() {
        const Status flushed = Flush();
        if (!flushed.has_value()) {
            return std::unexpected(flushed.error());
        }
        return last_sequence_;
    }

private:
    [[nodiscard]] Status Apply(ByteView record) {
        Result<WriteBatchReader> batch = WriteBatchReader::Open(record);
        if (!batch.has_value()) {
            return std::unexpected(Error::Corruption("log record is not a write batch: " +
                                                     std::string(batch.error().message())));
        }
        // Every batch takes the sequences after those of the batches before it; an
        // empty batch has the sequence that the next write takes.
        if (batch->sequence() <= last_sequence_) {
            return std::unexpected(
                Error::Corruption("log record's sequence " + std::to_string(batch->sequence()) +
                                  " does not follow " + std::to_string(last_sequence_)));
        }
        if (batch->count() == 0) {
            last_sequence_ = batch->sequence() - 1;
            return {};
        }
        const Status inserted = InsertBatch(*batch, *memtable_);
        // Sequences increase, so only a key over 4 GiB is rejected.
        if (!inserted.has_value()) {  // GCOVR_EXCL_BR_WITHOUT_HIT: 1/2 needs a key over 4 GiB
            return InvalidEntry(inserted.error());  // GCOVR_EXCL_LINE: needs a key over 4 GiB
        }
        last_sequence_ = batch->sequence() + batch->count() - 1;
        has_entries_ = true;
        if (memtable_->memory_usage() > options_->write_buffer_size) {
            return Flush();
        }
        return {};
    }

    [[nodiscard]] Status Flush() {
        if (!has_entries_) {
            return {};
        }
        const std::uint64_t number = versions_->NewFileNumber();
        // Tracked first, so that the table is evicted however building ends.
        built_->Add(number);
        Result<std::optional<FileMetadata>> table =
            BuildTable(*file_system_, *directory_, *comparator_, options_->table_options,
                       *table_cache_, *memtable_, number);
        if (!table.has_value()) {
            return std::unexpected(std::move(table).error());
        }
        FileMetadata file = std::move(*table).value();
        Expect(edit_->AddFile(0, std::move(file)));
        memtable_ = std::make_unique<MemTable>(comparator_->user_comparator());
        has_entries_ = false;
        return {};
    }

    FileSystem* file_system_;
    const std::filesystem::path* directory_;
    const InternalKeyComparator* comparator_;
    const RecoveryOptions* options_;
    TableCache* table_cache_;
    VersionSet* versions_;
    VersionEdit* edit_;
    BuiltTables* built_;
    std::unique_ptr<MemTable> memtable_;
    bool has_entries_ = false;
    // The last sequence that the replayed batches used.
    SequenceNumber last_sequence_ = 0;
};

}  // namespace

Result<RecoveredDatabase> RecoverDatabase(FileSystem& file_system,
                                          const std::filesystem::path& directory,
                                          const InternalKeyComparator& comparator,
                                          const RecoveryOptions& options, TableCache& table_cache) {
    const Status prepared = PrepareDirectory(file_system, directory, options);
    if (!prepared.has_value()) {
        return std::unexpected(prepared.error());
    }
    Result<std::unique_ptr<FileLock>> lock = file_system.LockFile(LockFileName(directory));
    if (!lock.has_value()) {
        return std::unexpected(std::move(lock).error());
    }
    Result<std::unique_ptr<VersionSet>> versions =
        OpenVersions(file_system, directory, comparator, options);
    if (!versions.has_value()) {
        return std::unexpected(std::move(versions).error());
    }
    VersionSet& set = **versions;
    const Result<std::vector<std::uint64_t>> logs =
        PrepareLogsForReplay(file_system, directory, set);
    if (!logs.has_value()) {
        return std::unexpected(logs.error());
    }

    VersionEdit edit;
    BuiltTables built(table_cache);
    LogReplay replay(file_system, directory, comparator, options, table_cache, set, edit, built);
    for (const std::uint64_t number : *logs) {
        const Status replayed = replay.ReplayLog(number);
        if (!replayed.has_value()) {
            return std::unexpected(replayed.error());
        }
    }
    const Result<SequenceNumber> last_sequence = replay.Finish();
    if (!last_sequence.has_value()) {
        return std::unexpected(last_sequence.error());
    }
    set.SetLastSequence(std::max(set.last_sequence(), *last_sequence));

    const std::uint64_t log_number = set.NewFileNumber();
    Result<std::unique_ptr<WritableFile>> log_file =
        file_system.OpenWritable(LogFileName(directory, log_number));
    if (!log_file.has_value()) {
        return std::unexpected(std::move(log_file).error());
    }
    auto log = std::make_unique<WalWriter>(std::move(*log_file));
    if (options.sync_wal_creation) {
        const Status log_synced = log->Sync();
        if (!log_synced.has_value()) {
            return std::unexpected(log_synced.error());
        }
        const Status directory_synced = file_system.SyncDirectory(directory);
        if (!directory_synced.has_value()) {
            return std::unexpected(directory_synced.error());
        }
    }
    edit.SetLogNumber(log_number);
    edit.SetPrevLogNumber(0);
    const Status applied = set.LogAndApply(std::move(edit));
    if (!applied.has_value()) {
        return std::unexpected(applied.error());
    }

    built.Keep();
    RecoveredDatabase recovered{
        .lock = std::move(*lock),
        .versions = std::move(*versions),
        .log = std::move(log),
        .log_number = log_number,
    };
    return recovered;
}

}  // namespace modern_leveldb
