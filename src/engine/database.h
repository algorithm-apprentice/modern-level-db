#ifndef MODERN_LEVELDB_ENGINE_DATABASE_H_
#define MODERN_LEVELDB_ENGINE_DATABASE_H_

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string_view>
#include <vector>

#include "engine/db_iterator.h"
#include "engine/dependency.h"
#include "engine/seek_statistics.h"
#include "engine/snapshot_registry.h"
#include "engine/table_cache.h"
#include "engine/write_buffer_state.h"
#include "engine/write_path.h"
#include "format/internal_key.h"
#include "format/write_batch.h"
#include "memory/memtable.h"
#include "metadata/version_edit.h"
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

struct CompactionHooks;

// Give compaction time before level-0 pressure becomes a hard stall: non-forced
// writes sleep a millisecond once when the current version reaches this count.
inline constexpr std::size_t WriteDelayLevel0FileCountThreshold = 8;
// At this level-0 count, a write that needs a new memtable waits for
// background work. Writes that still have room can proceed.
inline constexpr std::size_t WriteStallLevel0FileCountThreshold = 12;

struct DatabaseEngineOptions {
    // Must outlive the database.
    const Comparator* comparator = &BytewiseComparator();
    bool create_if_missing = false;
    bool error_if_exists = false;
    bool sync_wal_creation = true;
    // Soft threshold for memtable arena-reserved bytes; a write group can exceed it.
    std::size_t memtable_size_threshold = std::size_t{4} << 20U;
    // Compaction splits its outputs at this size.
    std::uint64_t max_file_size = std::uint64_t{2} << 20U;
    // Ten are kept for files other than tables.
    std::size_t max_open_files = 1000;
    bool allow_mmap_reads = true;
    TableBuilderOptions table_options{};
    // Without one, the database owns an 8 MiB block cache.
    BlockCache* block_cache = nullptr;
    // Without them, the database uses its admitted native file system, a serial
    // executor, and the system clock. Each must outlive the database. An executor must
    // queue tasks without running them in Schedule, and must eventually run
    // every task it accepts while the database is open.
    FileSystem* file_system = nullptr;
    BackgroundExecutor* executor = nullptr;
    Clock* clock = nullptr;
    // Applies only to the owned Windows backend, never to an injected filesystem.
    bool allow_weak_namespace_durability = false;

    // Clips resource policy to supported ranges: max_open_files to 74
    // through 50,000, memtable_size_threshold to 64 KiB through 1 GiB, max_file_size to
    // 1 MiB through 1 GiB, and the block size to 1 KiB through 4 MiB.
    void Sanitize() noexcept;
};

struct DatabaseEngineReadOptions {
    // The sequence of a GetSnapshot registration that is not yet released;
    // without one, reads see the last sequence.
    std::optional<SequenceNumber> snapshot;
    bool fill_cache = true;
};

struct DatabaseEngineLevelState {
    std::size_t file_count = 0;
    std::uint64_t file_bytes = 0;
};

struct DatabaseEngineState {
    std::array<DatabaseEngineLevelState, NumLevels> levels{};

    SequenceNumber last_sequence = 0;
    std::size_t snapshot_count = 0;
    std::optional<SequenceNumber> oldest_snapshot_sequence;

    std::size_t write_queue_depth = 0;
    std::size_t mutable_memtable_bytes = 0;
    std::optional<std::size_t> immutable_memtable_bytes;

    std::size_t protected_output_count = 0;
    bool background_work_scheduled = false;
    std::optional<Error> sticky_error;
};

// Coordinates visibility, durability, and reclamation. The mutex protects
// topology, queues, snapshots, and publication; serialized memtable insertion
// and file I/O can run outside it with retained sources.
// Methods support concurrent calls, never races with destruction.
class DatabaseEngine final {
    struct PrivateTag {
        explicit PrivateTag() = default;
    };

    struct EnginePolicy {
        std::size_t memtable_size_threshold;
        std::uint64_t max_file_size;
        bool sync_wal_creation;
        TableBuilderOptions table_options;
    };

public:
    using SnapshotRegistration = SnapshotRegistry::Registration;

    // Opens or creates the database in the directory, which it keeps locked.
    [[nodiscard]] static Result<std::unique_ptr<DatabaseEngine>> Open(
        DatabaseEngineOptions options, std::filesystem::path directory);

    DatabaseEngine(PrivateTag, const DatabaseEngineOptions& options,
                   std::filesystem::path directory, Dependency<FileSystem> file_system);
    DatabaseEngine(const DatabaseEngine&) = delete;
    DatabaseEngine& operator=(const DatabaseEngine&) = delete;
    DatabaseEngine(DatabaseEngine&&) = delete;
    DatabaseEngine& operator=(DatabaseEngine&&) = delete;
    // Marks the database as closing, closes the log, waits until no background
    // task is scheduled, and then releases the directory lock last. No other
    // call may be running.
    ~DatabaseEngine();

    // Commits the batch, syncing the log first if asked. After a sticky error,
    // every write returns it.
    [[nodiscard]] Status Write(EncodedWriteBatch& batch, bool sync);

    // Writes the key's value and returns true, or leaves it unchanged and
    // returns false if the key is missing or deleted.
    [[nodiscard]] Result<bool> Get(ByteView key, std::vector<std::byte>& value,
                                   const DatabaseEngineReadOptions& options = {});

    // Returns an iterator that must be destroyed before the database, since its
    // samples of what it reads charge the database's seek budgets.
    [[nodiscard]] std::unique_ptr<DbIterator> NewIterator(
        const DatabaseEngineReadOptions& options = {});

    // Registers the last sequence and keeps what it reads until the registration
    // is released. Each call returns a distinct registration.
    [[nodiscard]] SnapshotRegistration* GetSnapshot();
    void ReleaseSnapshot(SnapshotRegistration*& snapshot);

    // Returns an owning snapshot of the current topology and maintenance state.
    [[nodiscard]] Result<DatabaseEngineState> GetState();

    // Switches to a new memtable, even an empty one, and waits until the old one
    // is flushed. Returns the sticky error if there is one.
    [[nodiscard]] Status FlushMemTable();

    // Waits until no background task is scheduled, and returns the sticky error
    // if there is one.
    [[nodiscard]] Status WaitForBackgroundWork();

private:
    // Pins captured mutable/immutable memtables and a Version across unlocked reads.
    // Construction and destruction require the caller to hold the engine's mutex_.
    class ReadSources final {
    public:
        explicit ReadSources(DatabaseEngine& engine) noexcept;
        ReadSources(const ReadSources&) = delete;
        ReadSources& operator=(const ReadSources&) = delete;
        ~ReadSources();

        [[nodiscard]] const MemTable& mutable_memtable() const noexcept;
        [[nodiscard]] const MemTable* immutable_memtable() const noexcept;
        [[nodiscard]] const Version& version() const noexcept;

    private:
        DatabaseEngine* engine_;
        std::optional<MemTable::ReadPin> mutable_memtable_;
        std::optional<MemTable::ReadPin> immutable_memtable_;
        std::optional<VersionSet::ReadPin> version_;
    };

    [[nodiscard]] Status Recover(const DatabaseEngineOptions& options);
    [[nodiscard]] Status MakeRoomForWrite(std::unique_lock<std::mutex>& lock, bool force);
    [[nodiscard]] Status SwitchMemTable();
    struct PreparedRotation {
        std::unique_ptr<WalWriter> log;
        std::shared_ptr<MemTable> memtable;
        std::uint64_t log_number;
    };
    [[nodiscard]] Result<PreparedRotation> PrepareRotation();
    void CloseOldLogForRotation();
    [[nodiscard]] Status CommitWrite(std::unique_lock<std::mutex>& lock, EncodedWriteBatch& group,
                                     bool sync);
    [[nodiscard]] bool NeedsCompaction() const;
    void MaybeScheduleBackgroundWork();
    void BackgroundCall();
    void FlushImmutable(std::unique_lock<std::mutex>& lock);
    void BackgroundCompaction(std::unique_lock<std::mutex>& lock);
    // Runs the compaction that the current version needs, recording each output
    // number. Returns whether it ran one rather than moving a file.
    [[nodiscard]] bool Compact(std::unique_lock<std::mutex>& lock,
                               std::vector<std::uint64_t>& outputs);
    void InstallTrivialCompaction(const Compaction& compaction, std::unique_lock<std::mutex>& lock);
    [[nodiscard]] CompactionHooks MakeCompactionHooks(std::vector<std::uint64_t>& outputs);
    void RewriteCompaction(const Compaction& compaction, std::unique_lock<std::mutex>& lock,
                           std::vector<std::uint64_t>& outputs);
    [[nodiscard]] Status ApplyBackgroundEdit(Result<VersionEdit> edit,
                                             std::unique_lock<std::mutex>& lock,
                                             std::string_view closing_error);
    // Records the error of applying a compaction's edit, or forgets the seek
    // budgets that the new version no longer needs.
    void FinishCompaction(const Status& applied);
    // A compaction's check before each entry, which takes the mutex itself.
    [[nodiscard]] Status BeforeCompactionEntry();
    void RecordReadSample(ByteView internal_key);
    void RemoveObsoleteFiles(std::unique_lock<std::mutex>& lock);
    // Collects and evicts obsolete tables with mutex_ held; does not delete files.
    [[nodiscard]] Result<std::vector<std::filesystem::path>> CollectObsoleteFiles();
    void RecordStickyError(Error error);
    [[nodiscard]] std::unexpected<Error> StickyError() const;

    // Dependencies outlive their users; injected objects remain borrowed.
    Dependency<FileSystem> file_system_;
    Dependency<BlockCache> block_cache_;
    Dependency<Clock> clock_;
    std::unique_ptr<FileLock> lock_;

    const EnginePolicy policy_;
    std::filesystem::path directory_;
    InternalKeyComparator comparator_;
    TableCache table_cache_;

    std::mutex mutex_;
    std::condition_variable background_finished_;
    std::unique_ptr<VersionSet> versions_;
    WriteBufferState write_buffers_;
    WriteQueue write_queue_;
    SnapshotRegistry snapshots_;
    std::set<std::uint64_t> pending_outputs_;
    SeekStatistics seek_statistics_;
    // The last seed of an iterator's read sampling.
    std::uint32_t seed_ = 0;
    std::optional<Error> sticky_error_;
    bool background_scheduled_ = false;
    // Mirror state that a compaction checks without the mutex.
    std::atomic<bool> closing_ = false;

    // Destroyed first, so that it stops before anything that its tasks use.
    Dependency<BackgroundExecutor> executor_;
};

}  // namespace modern_leveldb

#endif  // MODERN_LEVELDB_ENGINE_DATABASE_H_
