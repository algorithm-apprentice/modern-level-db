#include "engine/build_table.h"

#include <cassert>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <utility>

#include "metadata/filenames.h"
#include "modern_leveldb/base/bytes.h"

namespace modern_leveldb {
namespace {

// Decodes a memtable key, which the table builder accepted as an internal key.
InternalKey Decoded(ByteView key) {
  Result<InternalKey> decoded = InternalKey::Decode(key);
  assert(decoded.has_value());
  return std::move(decoded).value();
}

// Adds the entries from the iterator's position to a table in the file and
// finishes the table, which closes the file. Returns the table's size and
// leaves the last added key in `largest`.
Result<std::uint64_t> WriteTable(std::unique_ptr<WritableFile> file,
                                 const InternalKeyComparator& comparator,
                                 const TableBuilderOptions& options, MemTable::Iterator& entry,
                                 ByteView& largest) {
  TableBuilder builder(std::move(file), comparator, options);
  for (; entry.valid(); entry.Next()) {
    largest = entry.key();
    if (!builder.Add(largest, entry.value()).has_value()) {
      break;
    }
  }
  // After an error from Add, Finish only closes the file and returns that error.
  const Status finished = builder.Finish();
  if (!finished.has_value()) {
    return std::unexpected(finished.error());
  }
  return builder.file_size();
}

}  // namespace

Result<std::optional<FileMetadata>> BuildTable(FileSystem& file_system,
                                               const std::filesystem::path& directory,
                                               const InternalKeyComparator& comparator,
                                               const TableBuilderOptions& options,
                                               TableCache& table_cache, const MemTable& memtable,
                                               std::uint64_t number) {
  MemTable::Iterator entry(memtable);
  entry.SeekToFirst();
  if (!entry.valid()) {
    return std::optional<FileMetadata>();
  }
  const std::filesystem::path path = TableFileName(directory, number);
  Result<std::unique_ptr<WritableFile>> file = file_system.OpenWritable(path);
  if (!file.has_value()) {
    return std::unexpected(std::move(file).error());
  }

  const ByteView smallest = entry.key();
  ByteView largest;
  Result<std::uint64_t> size = WriteTable(std::move(*file), comparator, options, entry, largest);
  if (size.has_value()) {
    const Result<TableCache::Handle> table = table_cache.Find(number, *size);
    if (!table.has_value()) {
      size = std::unexpected(table.error());
    }
  }
  if (!size.has_value()) {
    static_cast<void>(file_system.RemoveFile(path));
    return std::unexpected(std::move(size).error());
  }
  InternalKey smallest_key = Decoded(smallest);
  InternalKey largest_key = Decoded(largest);
  FileMetadata metadata{
      .number = number,
      .file_size = *size,
      .smallest = std::move(smallest_key),
      .largest = std::move(largest_key),
  };
  return std::optional<FileMetadata>(std::move(metadata));
}

}  // namespace modern_leveldb
