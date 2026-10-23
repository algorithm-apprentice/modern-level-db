#include "engine/database.h"

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "api/api_internal.h"
#include "api/multi_get.h"
#include "format/write_batch.h"
#include "modern_leveldb/db.h"
#include "table/bloom_filter.h"
#include "table/compression.h"

namespace modern_leveldb {
namespace {

DatabaseLevelState PublicLevelState(const DatabaseEngineLevelState& state) noexcept {
    return {
        .file_count = state.file_count,
        .file_bytes = state.file_bytes,
    };
}

DatabaseStateSnapshot PublicState(DatabaseEngineState state) {
    static_assert(DatabaseLevelCount == NumLevels);
    DatabaseStateSnapshot result;
    for (std::size_t level = 0; level < DatabaseLevelCount; ++level) {
        result.levels[level] = PublicLevelState(state.levels[level]);
    }
    result.last_sequence = state.last_sequence;
    result.snapshot_count = state.snapshot_count;
    result.oldest_snapshot_sequence = state.oldest_snapshot_sequence;
    result.write_queue_depth = state.write_queue_depth;
    result.mutable_memtable_bytes = state.mutable_memtable_bytes;
    result.immutable_memtable_bytes = state.immutable_memtable_bytes;
    result.protected_output_count = state.protected_output_count;
    result.background_work_scheduled = state.background_work_scheduled;
    result.sticky_error = std::move(state.sticky_error);
    return result;
}

Error MovedFromDatabase(std::string_view operation) {
    return Error::InvalidArgument(std::string(operation) + " used a moved-from database");
}

Result<DatabaseEngineReadOptions> PrepareReadOptions(detail::DatabaseState& state,
                                                     bool has_snapshot,
                                                     detail::SnapshotRegistration* snapshot,
                                                     bool fill_cache) {
    DatabaseEngineReadOptions prepared;
    prepared.fill_cache = fill_cache;
    if (!has_snapshot) {
        return prepared;
    }
    if (snapshot == nullptr || snapshot->state().get() != &state) {
        return std::unexpected(Error::InvalidArgument(
            "the read snapshot is moved from or belongs to another database"));
    }
    prepared.snapshot = snapshot->sequence();
    return prepared;
}

Result<BlockCompression> EngineCompression(Compression compression) {
    switch (compression) {
        case Compression::None:
            return BlockCompression::None;
        case Compression::Snappy:
            return BlockCompression::Snappy;
        case Compression::Zstd:
            return BlockCompression::Zstd;
    }
    return std::unexpected(Error::InvalidArgument("compression type is invalid"));
}

struct FilterSettings {
    std::optional<BloomFilterPolicy> policy;
    FilterLayout layout = FilterLayout::BlockBased;
};

Result<FilterSettings> PrepareFilterSettings(const Options& options) {
    FilterSettings result;
    switch (options.bloom_filter_layout) {
        case BloomFilterLayout::Automatic:
            result.layout = FilterLayout::FullTable;
            break;
        case BloomFilterLayout::BlockBased:
            result.layout = FilterLayout::BlockBased;
            break;
        case BloomFilterLayout::FullTable:
            result.layout = FilterLayout::FullTable;
            break;
        default:
            return std::unexpected(Error::InvalidArgument("Bloom filter layout is invalid"));
    }
    const bool custom_automatic = options.bloom_filter_layout == BloomFilterLayout::Automatic &&
                                  options.comparator != nullptr &&
                                  options.comparator.get() != &BytewiseComparator();
    if (options.bloom_bits_per_key.has_value() && !custom_automatic) {
        result.policy.emplace(*options.bloom_bits_per_key);
    }
    return result;
}

DatabaseEngineOptions EngineOptions(const Options& options, BlockCompression compression,
                                    const FilterSettings& filter) {
    DatabaseEngineOptions engine;
    engine.comparator =
        options.comparator != nullptr ? options.comparator.get() : &BytewiseComparator();
    engine.create_if_missing = options.create_if_missing;
    engine.error_if_exists = options.error_if_exists;
    engine.sync_wal_creation = options.sync_wal_creation;
    engine.allow_weak_namespace_durability = options.allow_weak_namespace_durability;
    engine.memtable_size_threshold = options.write_buffer_size;
    engine.max_file_size = options.max_file_size;
    engine.max_open_files = options.max_open_files;
    engine.allow_mmap_reads = options.allow_mmap_reads;
    engine.reuse_table_readers = options.reuse_table_readers;
    engine.file_system = options.file_system.get();
    engine.block_cache = options.block_cache.get();
    engine.clock = options.clock.get();
    engine.executor = options.executor.get();
    engine.table_options.block_size = options.block_size;
    engine.table_options.restart_interval = options.block_restart_interval;
    engine.table_options.compression = compression;
    engine.table_options.zstd_compression_level = options.zstd_compression_level;
    engine.table_options.filter_policy = filter.policy;
    engine.table_options.filter_layout = filter.layout;
    return engine;
}

Result<DatabaseEngineOptions> PrepareEngineOptions(const Options& options) {
    if (options.block_restart_interval == 0) {
        return std::unexpected(
            Error::InvalidArgument("block_restart_interval must be at least one"));
    }
    Result<BlockCompression> compression = EngineCompression(options.compression);
    if (!compression.has_value()) {
        return std::unexpected(compression.error());
    }
    const Status valid = ValidateCompressionOptions(*compression, options.zstd_compression_level);
    if (!valid.has_value()) {
        return std::unexpected(valid.error());
    }
    const Result<FilterSettings> filter = PrepareFilterSettings(options);
    if (!filter.has_value()) {
        return std::unexpected(filter.error());
    }
    return EngineOptions(options, *compression, *filter);
}

}  // namespace

Result<Database> Database::Open(Options options, std::filesystem::path directory) {
    Result<DatabaseEngineOptions> engine_options = PrepareEngineOptions(options);
    if (!engine_options.has_value()) {
        return std::unexpected(std::move(engine_options).error());
    }
    Result<std::unique_ptr<DatabaseEngine>> engine =
        DatabaseEngine::Open(std::move(*engine_options), std::move(directory));
    if (!engine.has_value()) {
        return std::unexpected(std::move(engine).error());
    }
    auto state = std::make_shared<detail::DatabaseState>(std::move(options), std::move(*engine));
    return Database(std::move(state));
}

Database::Database(std::shared_ptr<detail::DatabaseState> state) noexcept
    : state_(std::move(state)) {}

Database::Database(Database&& source) noexcept = default;

Database& Database::operator=(Database&& source) noexcept = default;

Database::~Database() = default;

Status Database::Put(ByteView key, ByteView value, const WriteOptions& options) {
    detail::DatabaseState* const state = state_.get();
    if (state == nullptr) {
        return std::unexpected(MovedFromDatabase("Put"));
    }
    EncodedWriteBatch batch;
    batch.Put(key, value);
    return state->engine().Write(batch, options.sync);
}

Status Database::Delete(ByteView key, const WriteOptions& options) {
    detail::DatabaseState* const state = state_.get();
    if (state == nullptr) {
        return std::unexpected(MovedFromDatabase("Delete"));
    }
    EncodedWriteBatch batch;
    batch.Delete(key);
    return state->engine().Write(batch, options.sync);
}

Status Database::Write(const WriteBatch& batch, const WriteOptions& options) {
    detail::DatabaseState* const state = state_.get();
    if (state == nullptr) {
        return std::unexpected(MovedFromDatabase("Write"));
    }
    if (batch.impl_ == nullptr) {
        return std::unexpected(Error::InvalidArgument("the write batch was moved from"));
    }
    EncodedWriteBatch copy = batch.impl_->batch();
    return state->engine().Write(copy, options.sync);
}

Status Database::WriteExclusive(WriteBatch& batch, const WriteOptions& options) {
    detail::DatabaseState* const state = state_.get();
    if (state == nullptr) {
        return std::unexpected(MovedFromDatabase("WriteExclusive"));
    }
    if (batch.impl_ == nullptr) {
        return std::unexpected(Error::InvalidArgument("the write batch was moved from"));
    }
    return state->engine().Write(batch.impl_->batch(), options.sync);
}

Result<bool> Database::Get(const ByteView& key, std::vector<std::byte>& value,
                           const ReadOptions& options) {
    detail::DatabaseState* const state = state_.get();
    if (state == nullptr) {
        return std::unexpected(MovedFromDatabase("Get"));
    }
    detail::SnapshotRegistration* const snapshot =
        options.snapshot != nullptr ? options.snapshot->registration_.get() : nullptr;
    const Result<DatabaseEngineReadOptions> prepared =
        PrepareReadOptions(*state, options.snapshot != nullptr, snapshot, options.fill_cache);
    if (!prepared.has_value()) {
        return std::unexpected(prepared.error());
    }
    return state->engine().Get(key, value, *prepared);
}

Result<std::optional<std::vector<std::byte>>> Database::Get(const ByteView& key,
                                                            const ReadOptions& options) {
    std::vector<std::byte> value;
    Result<bool> found = Get(key, value, options);
    if (!found.has_value()) {
        return std::unexpected(std::move(found).error());
    }
    if (!*found) {
        return std::optional<std::vector<std::byte>>();
    }
    return std::optional<std::vector<std::byte>>(std::move(value));
}

Result<std::vector<OwningReadResult>> Database::MultiGet(std::span<const ByteView> keys,
                                                         const MultiGetOptions& options) {
    const auto state = state_;
    if (state == nullptr) {
        return std::unexpected(MovedFromDatabase("MultiGet"));
    }
    if (options.max_concurrency < 1 || options.max_concurrency > 16) {
        return std::unexpected(
            Error::InvalidArgument("MultiGet max_concurrency must be in [1, 16]"));
    }
    auto snapshot = options.read_options.snapshot != nullptr
                        ? options.read_options.snapshot->registration_
                        : nullptr;
    const auto prepared = PrepareReadOptions(*state, options.read_options.snapshot != nullptr,
                                             snapshot.get(), options.read_options.fill_cache);
    if (!prepared.has_value()) {
        return std::unexpected(prepared.error());
    }
    return detail::ReadBatch(state, keys, *prepared, std::move(snapshot), options.max_concurrency);
}

Result<Iterator> Database::NewIterator(const ReadOptions& options) {
    const std::shared_ptr<detail::DatabaseState> state = state_;
    if (state == nullptr) {
        return std::unexpected(MovedFromDatabase("NewIterator"));
    }
    std::shared_ptr<detail::SnapshotRegistration> snapshot =
        options.snapshot != nullptr ? options.snapshot->registration_ : nullptr;
    const Result<DatabaseEngineReadOptions> prepared =
        PrepareReadOptions(*state, options.snapshot != nullptr, snapshot.get(), options.fill_cache);
    if (!prepared.has_value()) {
        return std::unexpected(prepared.error());
    }
    std::unique_ptr<DbIterator> iterator = state->engine().NewIterator(*prepared);
    auto impl = std::make_unique<Iterator::Impl>(state, std::move(snapshot), std::move(iterator));
    return Iterator(std::move(impl));
}

Result<Snapshot> Database::GetSnapshot() {
    const std::shared_ptr<detail::DatabaseState> state = state_;
    if (state == nullptr) {
        return std::unexpected(MovedFromDatabase("GetSnapshot"));
    }
    auto registration = std::make_shared<detail::SnapshotRegistration>(state);
    return Snapshot(std::move(registration));
}

Result<DatabaseStateSnapshot> Database::GetState() {
    detail::DatabaseState* const state = state_.get();
    if (state == nullptr) {
        return std::unexpected(MovedFromDatabase("GetState"));
    }
    return state->engine().GetState().transform(
        [](DatabaseEngineState captured) { return PublicState(std::move(captured)); });
}

}  // namespace modern_leveldb
