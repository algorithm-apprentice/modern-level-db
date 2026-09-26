#include "engine/database.h"

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "api/api_internal.h"
#include "format/write_batch.h"
#include "modern_leveldb/db.h"
#include "table/bloom_filter.h"
#include "table/compression.h"

namespace modern_leveldb {
namespace {

struct PreparedRead {
  DatabaseEngineReadOptions options;
  std::shared_ptr<detail::SnapshotRegistration> snapshot;
};

Error MovedFromDatabase(std::string_view operation) {
  return Error::InvalidArgument(std::string(operation) + " used a moved-from database");
}

Result<PreparedRead> PrepareRead(const std::shared_ptr<detail::DatabaseState>& state,
                                 bool has_snapshot,
                                 std::shared_ptr<detail::SnapshotRegistration> snapshot,
                                 bool fill_cache) {
  PreparedRead prepared;
  prepared.options.fill_cache = fill_cache;
  if (!has_snapshot) {
    return prepared;
  }
  prepared.snapshot = std::move(snapshot);
  if (prepared.snapshot == nullptr || prepared.snapshot->state().get() != state.get()) {
    return std::unexpected(
        Error::InvalidArgument("the read snapshot is moved from or belongs to another database"));
  }
  prepared.options.snapshot = prepared.snapshot->sequence();
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

DatabaseEngineOptions EngineOptions(const Options& options, BlockCompression compression) {
  DatabaseEngineOptions engine;
  engine.comparator =
      options.comparator != nullptr ? options.comparator.get() : &BytewiseComparator();
  engine.create_if_missing = options.create_if_missing;
  engine.error_if_exists = options.error_if_exists;
  engine.write_buffer_size = options.write_buffer_size;
  engine.max_file_size = options.max_file_size;
  engine.max_open_files = options.max_open_files;
  engine.table_options.block_size = options.block_size;
  engine.table_options.restart_interval = options.block_restart_interval;
  engine.table_options.compression = compression;
  engine.table_options.zstd_compression_level = options.zstd_compression_level;
  if (options.bloom_bits_per_key.has_value()) {
    engine.table_options.filter_policy.emplace(*options.bloom_bits_per_key);
  }
  return engine;
}

}  // namespace

Result<Database> Database::Open(Options options, std::filesystem::path directory) {
  if (options.block_restart_interval == 0) {
    return std::unexpected(Error::InvalidArgument("block_restart_interval must be at least one"));
  }
  Result<BlockCompression> compression = EngineCompression(options.compression);
  if (!compression.has_value()) {
    return std::unexpected(compression.error());
  }
  const Status valid = ValidateCompressionOptions(*compression, options.zstd_compression_level);
  if (!valid.has_value()) {
    return std::unexpected(valid.error());
  }
  DatabaseEngineOptions engine_options = EngineOptions(options, *compression);
  Result<std::unique_ptr<DatabaseEngine>> engine =
      DatabaseEngine::Open(std::move(engine_options), std::move(directory));
  if (!engine.has_value()) {
    return std::unexpected(std::move(engine).error());
  }
  auto state =
      std::make_shared<detail::DatabaseState>(std::move(options.comparator), std::move(*engine));
  return Database(std::move(state));
}

Database::Database(std::shared_ptr<detail::DatabaseState> state) noexcept
    : state_(std::move(state)) {}

Database::Database(Database&& source) noexcept = default;

Database& Database::operator=(Database&& source) noexcept = default;

Database::~Database() = default;

Status Database::Put(ByteView key, ByteView value, const WriteOptions& options) {
  const std::shared_ptr<detail::DatabaseState> state = state_;
  if (state == nullptr) {
    return std::unexpected(MovedFromDatabase("Put"));
  }
  EncodedWriteBatch batch;
  const Status added = batch.Put(key, value);
  if (!added.has_value()) {  // GCOVR_EXCL_BR_WITHOUT_HIT: 1/2 needs over 4 GiB
    return added;            // GCOVR_EXCL_LINE: needs a key or value over 4 GiB
  }
  return state->engine().Write(batch, options.sync);
}

Status Database::Delete(ByteView key, const WriteOptions& options) {
  const std::shared_ptr<detail::DatabaseState> state = state_;
  if (state == nullptr) {
    return std::unexpected(MovedFromDatabase("Delete"));
  }
  EncodedWriteBatch batch;
  const Status added = batch.Delete(key);
  if (!added.has_value()) {  // GCOVR_EXCL_BR_WITHOUT_HIT: 1/2 needs over 4 GiB
    return added;            // GCOVR_EXCL_LINE: needs a key over 4 GiB
  }
  return state->engine().Write(batch, options.sync);
}

Status Database::Write(const WriteBatch& batch, const WriteOptions& options) {
  const std::shared_ptr<detail::DatabaseState> state = state_;
  if (state == nullptr) {
    return std::unexpected(MovedFromDatabase("Write"));
  }
  if (batch.impl_ == nullptr) {
    return std::unexpected(Error::InvalidArgument("the write batch was moved from"));
  }
  return state->engine().Write(batch.impl_->batch(), options.sync);
}

Result<std::optional<std::vector<std::byte>>> Database::Get(ByteView key,
                                                            const ReadOptions& options) {
  const std::shared_ptr<detail::DatabaseState> state = state_;
  if (state == nullptr) {
    return std::unexpected(MovedFromDatabase("Get"));
  }
  std::shared_ptr<detail::SnapshotRegistration> snapshot =
      options.snapshot != nullptr ? options.snapshot->registration_ : nullptr;
  Result<PreparedRead> prepared =
      PrepareRead(state, options.snapshot != nullptr, std::move(snapshot), options.fill_cache);
  if (!prepared.has_value()) {
    return std::unexpected(std::move(prepared).error());
  }
  return state->engine().Get(key, prepared->options);
}

Result<Iterator> Database::NewIterator(const ReadOptions& options) {
  const std::shared_ptr<detail::DatabaseState> state = state_;
  if (state == nullptr) {
    return std::unexpected(MovedFromDatabase("NewIterator"));
  }
  std::shared_ptr<detail::SnapshotRegistration> snapshot =
      options.snapshot != nullptr ? options.snapshot->registration_ : nullptr;
  Result<PreparedRead> prepared =
      PrepareRead(state, options.snapshot != nullptr, std::move(snapshot), options.fill_cache);
  if (!prepared.has_value()) {
    return std::unexpected(std::move(prepared).error());
  }
  std::unique_ptr<DbIterator> iterator = state->engine().NewIterator(prepared->options);
  auto impl =
      std::make_unique<Iterator::Impl>(state, std::move(prepared->snapshot), std::move(iterator));
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

}  // namespace modern_leveldb
