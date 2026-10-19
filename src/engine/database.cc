#include "engine/database.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <mutex>
#include <utility>

#include "engine/recovery.h"
#include "platform/file_system.h"

namespace modern_leveldb {
namespace {

// Reserve this many open-file budget slots
// for non-table files before sizing the table cache, not an actual file count.
constexpr std::size_t ReservedNonTableFileCount = 10;
// The block cache that a database owns when it is given none.
constexpr std::size_t DefaultBlockCacheSize = std::size_t{8} << 20U;

Result<Dependency<FileSystem>> PrepareFileSystem(const DatabaseEngineOptions& options,
                                                 std::filesystem::path& directory) {
    if (options.file_system != nullptr) {
        return BorrowDependency(*options.file_system);
    }
    return CreateDefaultFileSystem(directory, options.allow_weak_namespace_durability,
                                   options.allow_mmap_reads)
        .transform([](std::unique_ptr<FileSystem> file_system) {
            return OwnDependency(std::move(file_system));
        });
}

Dependency<BlockCache> PrepareBlockCache(const DatabaseEngineOptions& options) {
    if (options.block_cache != nullptr) {
        return BorrowDependency(*options.block_cache);
    }
    return OwnDependency(std::make_unique<BlockCache>(DefaultBlockCacheSize));
}

Dependency<BackgroundExecutor> PrepareExecutor(const DatabaseEngineOptions& options) {
    if (options.executor != nullptr) {
        return BorrowDependency(*options.executor);
    }
    return OwnDependency<BackgroundExecutor>(std::make_unique<SerialExecutor>());
}

Dependency<Clock> PrepareClock(const DatabaseEngineOptions& options) {
    if (options.clock != nullptr) {
        return BorrowDependency(*options.clock);
    }
    return OwnDependency<Clock>(std::make_unique<SystemClock>());
}

TableOptions ReadTableOptions(const DatabaseEngineOptions& options, BlockCache* block_cache) {
    TableOptions table_options{.filter_policy = options.table_options.filter_policy,
                               .block_cache = block_cache};
    return table_options;
}

}  // namespace

void DatabaseEngineOptions::Sanitize() noexcept {
    max_open_files = std::clamp<std::size_t>(max_open_files, 64 + ReservedNonTableFileCount, 50000);
    memtable_size_threshold = std::clamp<std::size_t>(
        memtable_size_threshold, std::size_t{64} << 10U, std::size_t{1} << 30U);
    max_file_size =
        std::clamp<std::uint64_t>(max_file_size, std::uint64_t{1} << 20U, std::uint64_t{1} << 30U);
    table_options.block_size = std::clamp<std::size_t>(
        table_options.block_size, std::size_t{1} << 10U, std::size_t{4} << 20U);
}

Result<std::unique_ptr<DatabaseEngine>> DatabaseEngine::Open(DatabaseEngineOptions options,
                                                             std::filesystem::path directory) {
    return PrepareFileSystem(options, directory)
        .and_then(
            [&](Dependency<FileSystem> file_system) -> Result<std::unique_ptr<DatabaseEngine>> {
                options.Sanitize();
                auto database = std::make_unique<DatabaseEngine>(
                    PrivateTag(), options, std::move(directory), std::move(file_system));
                const Status recovered = database->Recover(options);
                if (!recovered.has_value()) {
                    return std::unexpected(recovered.error());
                }
                return database;
            });
}

DatabaseEngine::DatabaseEngine(PrivateTag, const DatabaseEngineOptions& options,
                               std::filesystem::path directory, Dependency<FileSystem> file_system)
    : file_system_(std::move(file_system)),
      block_cache_(PrepareBlockCache(options)),
      clock_(PrepareClock(options)),
      policy_{.memtable_size_threshold = options.memtable_size_threshold,
              .max_file_size = options.max_file_size,
              .sync_wal_creation = options.sync_wal_creation,
              .table_options = options.table_options},  // GCOVR_EXCL_LINE: GCC constructor clone
      directory_(std::move(directory)),
      comparator_(*options.comparator),
      table_cache_(*file_system_.get(), directory_, comparator_,
                   ReadTableOptions(options, block_cache_.get()),
                   options.max_open_files - ReservedNonTableFileCount),
      executor_(PrepareExecutor(options)) {}

DatabaseEngine::~DatabaseEngine() {
    std::unique_lock lock(mutex_);
    closing_ = true;
    // No write can use the log any longer. Destruction has no caller to receive
    // a close error; resource release is not a data-durability acknowledgment.
    if (write_buffers_.has_log()) {
        try {
            static_cast<void>(write_buffers_.log().Close());
        } catch (...) {
            // Destructors cannot report close failures; continue releasing resources.
        }
    }
    background_finished_.wait(lock, [this] { return !background_scheduled_; });
}

Status DatabaseEngine::Recover(const DatabaseEngineOptions& options) {
    RecoveryOptions recovery{.create_if_missing = options.create_if_missing,
                             .error_if_exists = options.error_if_exists,
                             .sync_wal_creation = options.sync_wal_creation,
                             .memtable_size_threshold = options.memtable_size_threshold,
                             .table_options = options.table_options};
    Result<RecoveredDatabase> recovered =
        RecoverDatabase(*file_system_.get(), directory_, comparator_, recovery, table_cache_);
    if (!recovered.has_value()) {
        return std::unexpected(std::move(recovered).error());
    }
    std::unique_lock lock(mutex_);
    lock_ = std::move(recovered->lock);
    versions_ = std::move(recovered->versions);
    write_buffers_.InstallRecovered(std::move(recovered->log), recovered->log_number,
                                    std::make_shared<MemTable>(comparator_.user_comparator()));
    RemoveObsoleteFiles(lock);
    // Only a task that cannot be scheduled records an error here.
    MaybeScheduleBackgroundWork();
    if (sticky_error_.has_value()) {
        return StickyError();
    }
    return {};
}

}  // namespace modern_leveldb
