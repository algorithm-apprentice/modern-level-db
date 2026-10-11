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

Result<std::unique_ptr<FileSystem>> PrepareOwnedFileSystem(const DatabaseEngineOptions& options,
                                                           std::filesystem::path& directory) {
    if (options.file_system != nullptr) {
        return std::unique_ptr<FileSystem>{};
    }
    return CreateDefaultFileSystem(directory, options.allow_weak_namespace_durability,
                                   options.allow_mmap_reads);
}

std::unique_ptr<BlockCache> OwnedBlockCache(const DatabaseEngineOptions& options) {
    if (options.block_cache != nullptr) {
        return nullptr;
    }
    return std::make_unique<BlockCache>(DefaultBlockCacheSize);
}

std::unique_ptr<BackgroundExecutor> OwnedExecutor(const DatabaseEngineOptions& options) {
    if (options.executor != nullptr) {
        return nullptr;
    }
    return std::make_unique<SerialExecutor>();
}

std::unique_ptr<Clock> OwnedClock(const DatabaseEngineOptions& options) {
    if (options.clock != nullptr) {
        return nullptr;
    }
    return std::make_unique<SystemClock>();
}

TableOptions ReadTableOptions(const DatabaseEngineOptions& options, BlockCache* owned_block_cache) {
    TableOptions table_options{
        .filter_policy = options.table_options.filter_policy,
        .block_cache = options.block_cache != nullptr ? options.block_cache : owned_block_cache};
    return table_options;
}

}  // namespace

DatabaseEngineOptions SanitizeOptions(DatabaseEngineOptions options) {
    options.max_open_files =
        std::clamp<std::size_t>(options.max_open_files, 64 + ReservedNonTableFileCount, 50000);
    options.write_buffer_size = std::clamp<std::size_t>(
        options.write_buffer_size, std::size_t{64} << 10U, std::size_t{1} << 30U);
    options.max_file_size = std::clamp<std::uint64_t>(
        options.max_file_size, std::uint64_t{1} << 20U, std::uint64_t{1} << 30U);
    options.table_options.block_size = std::clamp<std::size_t>(
        options.table_options.block_size, std::size_t{1} << 10U, std::size_t{4} << 20U);
    return options;
}

Result<std::unique_ptr<DatabaseEngine>> DatabaseEngine::Open(DatabaseEngineOptions options,
                                                             std::filesystem::path directory) {
    return PrepareOwnedFileSystem(options, directory)
        .and_then([&](std::unique_ptr<FileSystem> file_system)
                      -> Result<std::unique_ptr<DatabaseEngine>> {
            options = SanitizeOptions(std::move(options));
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
                               std::filesystem::path directory,
                               std::unique_ptr<FileSystem> owned_file_system)
    : owned_file_system_(std::move(owned_file_system)),
      owned_block_cache_(OwnedBlockCache(options)),
      owned_clock_(OwnedClock(options)),
      policy_{.write_buffer_size = options.write_buffer_size,
              .max_file_size = options.max_file_size,
              .sync_wal_creation = options.sync_wal_creation,
              .table_options = options.table_options},  // GCOVR_EXCL_LINE: GCC constructor clone
      directory_(std::move(directory)),
      file_system_(options.file_system != nullptr ? options.file_system : owned_file_system_.get()),
      clock_(options.clock != nullptr ? options.clock : owned_clock_.get()),
      comparator_(*options.comparator),
      table_cache_(*file_system_, directory_, comparator_,
                   ReadTableOptions(options, owned_block_cache_.get()),
                   options.max_open_files - ReservedNonTableFileCount),
      owned_executor_(OwnedExecutor(options)),
      executor_(options.executor != nullptr ? options.executor : owned_executor_.get()) {}

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
                             .write_buffer_size = options.write_buffer_size,
                             .table_options = options.table_options};
    Result<RecoveredDatabase> recovered =
        RecoverDatabase(*file_system_, directory_, comparator_, recovery, table_cache_);
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
    if (background_error_.has_value()) {
        return BackgroundError();
    }
    return {};
}

}  // namespace modern_leveldb
