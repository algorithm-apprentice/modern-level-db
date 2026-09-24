#include "engine/table_cache.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <utility>

#include "metadata/filenames.h"
#include "modern_leveldb/base/coding.h"

namespace modern_leveldb {
namespace {

std::array<std::byte, sizeof(std::uint64_t)> CacheKey(std::uint64_t file_number) noexcept {
  std::array<std::byte, sizeof(std::uint64_t)> key{};
  EncodeFixed64(key, file_number);
  return key;
}

}  // namespace

TableCache::TableCache(FileSystem& file_system, std::filesystem::path directory,
                       const InternalKeyComparator& comparator, const TableOptions& options,
                       std::size_t capacity)
    : file_system_(&file_system),
      directory_(std::move(directory)),
      comparator_(&comparator),
      options_(options),
      tables_(capacity) {}

Result<TableCache::Handle> TableCache::Find(std::uint64_t file_number, std::uint64_t file_size) {
  const auto key = CacheKey(file_number);
  std::optional<Handle> cached = tables_.Lookup(key);
  if (cached.has_value()) {
    return std::move(*cached);
  }

  Result<std::unique_ptr<RandomAccessFile>> file =
      file_system_->OpenRandomAccess(TableFileName(directory_, file_number));
  if (!file.has_value()) {
    return std::unexpected(std::move(file).error());
  }
  Result<std::unique_ptr<Table>> table =
      Table::Open(std::move(*file), file_size, *comparator_, options_);
  if (!table.has_value()) {
    return std::unexpected(std::move(table).error());
  }
  // Tables have charge one, so the charge accounting could overflow only with
  // SIZE_MAX tables in one shard; Insert would return that error.
  return tables_.Insert(key, std::shared_ptr<const Table>(std::move(*table)), 1);
}

void TableCache::Evict(std::uint64_t file_number) { tables_.Erase(CacheKey(file_number)); }

}  // namespace modern_leveldb
