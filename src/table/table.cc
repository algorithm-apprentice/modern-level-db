#include "table/table.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "format/internal_key.h"
#include "modern_leveldb/base/bytes.h"
#include "modern_leveldb/base/coding.h"
#include "modern_leveldb/base/comparator.h"
#include "modern_leveldb/base/result.h"
#include "platform/file_system.h"
#include "table/block.h"
#include "table/block_format.h"
#include "table/bloom_filter.h"
#include "table/filter_block.h"

namespace modern_leveldb {
namespace {

// Reads output.size() bytes, repeating short reads.
Status ReadExactly(const RandomAccessFile& file, std::uint64_t offset, MutableByteView output) {
  while (!output.empty()) {
    const Result<std::size_t> read = file.Read(offset, output);
    if (!read.has_value()) {
      return std::unexpected(read.error());
    }
    if (*read > output.size()) {
      return std::unexpected(Error::Io("random-access file returned an oversized table read"));
    }
    if (*read == 0) {
      return std::unexpected(Error::Corruption("table file is truncated"));
    }
    offset += *read;
    output = output.subspan(*read);
  }
  return {};
}

// Reports whether the block and its trailer end before the footer and fit in
// memory.
bool InBlockRegion(BlockHandle handle, std::uint64_t blocks_end) noexcept {
  if (handle.offset > blocks_end) {
    return false;
  }
  const std::uint64_t available =
      std::min<std::uint64_t>(blocks_end - handle.offset, std::numeric_limits<std::size_t>::max());
  return available >= BlockTrailerSize && handle.size <= available - BlockTrailerSize;
}

Result<std::vector<std::byte>> ReadStoredBlock(const RandomAccessFile& file,
                                               std::uint64_t blocks_end, BlockHandle handle) {
  if (!InBlockRegion(handle, blocks_end)) {
    return std::unexpected(Error::Corruption("table block lies outside the file"));
  }
  std::vector<std::byte> stored(static_cast<std::size_t>(handle.size) + BlockTrailerSize);
  const Status read = ReadExactly(file, handle.offset, stored);
  if (!read.has_value()) {
    return std::unexpected(read.error());
  }
  return DecodeStoredBlock(std::move(stored));
}

Result<Block> ReadBlock(const RandomAccessFile& file, std::uint64_t blocks_end, BlockHandle handle,
                        const Comparator& comparator) {
  Result<std::vector<std::byte>> contents = ReadStoredBlock(file, blocks_end, handle);
  if (!contents.has_value()) {
    return std::unexpected(std::move(contents).error());
  }
  return Block::Create(std::move(*contents), comparator);
}

// Decodes an index value, which Open validated.
BlockHandle IndexHandle(ByteView value) {
  const Result<BlockHandle> handle = ConsumeBlockHandle(value);
  assert(handle.has_value());
  return handle.value();
}

// Checks that the index values are handles of blocks before the footer that
// follow one another without overlapping, so that an offset identifies a block
// in the block cache.
Status ValidateIndex(const Block& index, std::uint64_t blocks_end) {
  std::uint64_t next_offset = 0;
  Block::Iterator entry(index);
  for (entry.SeekToFirst(); entry.valid(); entry.Next()) {
    ByteView value = entry.value();
    const Result<BlockHandle> handle = ConsumeBlockHandle(value);
    if (!handle.has_value() || !value.empty() || !InBlockRegion(*handle, blocks_end)) {
      return std::unexpected(Error::Corruption("table index value is not a block handle"));
    }
    if (handle->offset < next_offset) {
      return std::unexpected(Error::Corruption("table index blocks overlap"));
    }
    next_offset = handle->offset + handle->size + BlockTrailerSize;
  }
  return {};
}

// Reads the filter block that the metaindex maps to the policy, if any.
Status ReadFilter(const RandomAccessFile& file, std::uint64_t blocks_end, BlockHandle metaindex,
                  BloomFilterPolicy policy, std::optional<FilterBlockReader>& filter) {
  const Result<Block> meta = ReadBlock(file, blocks_end, metaindex, BytewiseComparator());
  if (!meta.has_value()) {
    return std::unexpected(meta.error());
  }
  std::string key = "filter.";
  key.append(policy.Name());
  Block::Iterator entry(*meta);
  entry.Seek(AsBytes(key));
  if (!entry.valid() || AsStringView(entry.key()) != key) {
    return {};
  }

  ByteView value = entry.value();
  const Result<BlockHandle> handle = ConsumeBlockHandle(value);
  if (!handle.has_value() || !value.empty()) {
    return std::unexpected(Error::Corruption("table filter handle is malformed"));
  }
  Result<std::vector<std::byte>> contents = ReadStoredBlock(file, blocks_end, *handle);
  if (!contents.has_value()) {
    return std::unexpected(std::move(contents).error());
  }
  Result<FilterBlockReader> reader = FilterBlockReader::Create(std::move(*contents), policy);
  if (!reader.has_value()) {
    return std::unexpected(std::move(reader).error());
  }
  filter.emplace(std::move(*reader));
  return {};
}

}  // namespace

Result<std::unique_ptr<Table>> Table::Open(std::unique_ptr<RandomAccessFile> file,
                                           std::uint64_t file_size,
                                           const InternalKeyComparator& comparator,
                                           const TableOptions& options) {
  if (file == nullptr) {
    return std::unexpected(Error::InvalidArgument("table has no file"));
  }
  if (file_size < FooterSize) {
    return std::unexpected(Error::Corruption("file is too short to be a table"));
  }
  const std::uint64_t blocks_end = file_size - FooterSize;
  std::array<std::byte, FooterSize> footer_bytes{};
  const Status read = ReadExactly(*file, blocks_end, footer_bytes);
  if (!read.has_value()) {
    return std::unexpected(read.error());
  }
  const Result<Footer> footer = DecodeFooter(footer_bytes);
  if (!footer.has_value()) {
    return std::unexpected(footer.error());
  }

  Result<Block> index = ReadBlock(*file, blocks_end, footer->index, comparator);
  if (!index.has_value()) {
    return std::unexpected(std::move(index).error());
  }
  const Status valid = ValidateIndex(*index, blocks_end);
  if (!valid.has_value()) {
    return std::unexpected(valid.error());
  }

  std::optional<FilterBlockReader> filter;
  if (options.filter_policy.has_value()) {
    const Status loaded =
        ReadFilter(*file, blocks_end, footer->metaindex, *options.filter_policy, filter);
    if (!loaded.has_value()) {
      return std::unexpected(loaded.error());
    }
  }

  const std::uint64_t cache_id = options.block_cache != nullptr ? options.block_cache->NewId() : 0;
  return std::unique_ptr<Table>(new Table(std::move(file), blocks_end, comparator,
                                          std::move(*index), std::move(filter), options.block_cache,
                                          cache_id));
}

Table::Table(std::unique_ptr<RandomAccessFile> file, std::uint64_t blocks_end,
             const InternalKeyComparator& comparator, Block index,
             std::optional<FilterBlockReader> filter, BlockCache* block_cache,
             std::uint64_t cache_id) noexcept
    : file_(std::move(file)),
      blocks_end_(blocks_end),
      comparator_(&comparator),
      index_(std::move(index)),
      filter_(std::move(filter)),
      block_cache_(block_cache),
      cache_id_(cache_id) {}

Result<std::optional<TableLookup>> Table::Get(const LookupKey& key,
                                              const TableReadOptions& options) const {
  Block::Iterator index(index_);
  index.Seek(key.internal_key());
  if (!index.valid()) {
    return std::optional<TableLookup>();
  }
  const BlockHandle handle = IndexHandle(index.value());
  if (filter_.has_value() && !filter_->KeyMayMatch(handle.offset, key.user_key())) {
    return std::optional<TableLookup>();
  }

  const Result<BlockReference> block = ReadDataBlock(handle, options);
  if (!block.has_value()) {
    return std::unexpected(block.error());
  }
  Block::Iterator entry(block->block());
  entry.Seek(key.internal_key());
  if (!entry.valid()) {
    return std::optional<TableLookup>();
  }
  // The comparator orders invalid internal keys before every valid one, such
  // as the lookup key, so the entry is valid.
  const ParsedInternalKey parsed = ParseInternalKey(entry.key()).value();
  if (comparator_->user_comparator().Compare(parsed.user_key, key.user_key()) != 0) {
    return std::optional<TableLookup>();
  }
  TableLookup lookup{
      .kind = parsed.kind,
      .value = std::vector<std::byte>(entry.value().begin(), entry.value().end()),
  };
  return lookup;
}

Result<Table::BlockReference> Table::ReadDataBlock(BlockHandle handle,
                                                   const TableReadOptions& options) const {
  std::array<std::byte, 2 * sizeof(std::uint64_t)> cache_key{};
  if (block_cache_ != nullptr) {
    EncodeFixed64(std::span(cache_key).first<sizeof(std::uint64_t)>(), cache_id_);
    EncodeFixed64(std::span(cache_key).last<sizeof(std::uint64_t)>(), handle.offset);
    std::optional<BlockCache::Handle> cached = block_cache_->Lookup(cache_key);
    if (cached.has_value()) {
      return BlockReference(std::move(*cached));
    }
  }

  Result<Block> block = ReadBlock(*file_, blocks_end_, handle, *comparator_);
  if (!block.has_value()) {
    return std::unexpected(std::move(block).error());
  }
  if (block->empty()) {
    return std::unexpected(Error::Corruption("table data block is empty"));
  }
  auto shared = std::make_shared<const Block>(std::move(*block));
  if (block_cache_ != nullptr && options.fill_cache) {
    // ReadBlock checked that the size fits in memory.
    Result<BlockCache::Handle> inserted =
        block_cache_->Insert(cache_key, shared, static_cast<std::size_t>(handle.size));
    // A cache whose charge accounting would overflow leaves the block uncached.
    if (inserted.has_value()) {
      return BlockReference(std::move(*inserted));
    }
  }
  return BlockReference(std::move(shared));
}

Table::Iterator::Iterator(const Table& table, const TableReadOptions& options) noexcept
    : table_(&table), options_(options), index_(table.index_) {}

ByteView Table::Iterator::key() const noexcept {
  assert(valid());
  return data_->key();
}

ByteView Table::Iterator::value() const noexcept {
  assert(valid());
  return data_->value();
}

Status Table::Iterator::SeekToFirst() {
  index_.SeekToFirst();
  return EnterBlock(Edge::First);
}

Status Table::Iterator::SeekToLast() {
  index_.SeekToLast();
  return EnterBlock(Edge::Last);
}

Status Table::Iterator::Seek(ByteView target) {
  index_.Seek(target);
  const Status loaded = LoadBlock();
  if (!loaded.has_value()) {
    return loaded;
  }
  if (!data_.has_value()) {
    return {};
  }
  data_->Seek(target);
  if (data_->valid()) {
    return {};
  }
  // The target follows the block's last key, and the next block starts after
  // the target.
  index_.Next();
  return EnterBlock(Edge::First);
}

Status Table::Iterator::Next() {
  assert(valid());
  data_->Next();
  if (data_->valid()) {
    return {};
  }
  index_.Next();
  return EnterBlock(Edge::First);
}

Status Table::Iterator::Prev() {
  assert(valid());
  data_->Prev();
  if (data_->valid()) {
    return {};
  }
  index_.Prev();
  return EnterBlock(Edge::Last);
}

Status Table::Iterator::LoadBlock() {
  data_.reset();
  block_.reset();
  if (!index_.valid()) {
    return {};
  }
  Result<BlockReference> block = table_->ReadDataBlock(IndexHandle(index_.value()), options_);
  if (!block.has_value()) {
    return std::unexpected(std::move(block).error());
  }
  block_.emplace(std::move(*block));
  data_.emplace(block_->block());
  return {};
}

Status Table::Iterator::EnterBlock(Edge edge) {
  const Status loaded = LoadBlock();
  if (!loaded.has_value()) {
    return loaded;
  }
  if (data_.has_value()) {
    if (edge == Edge::First) {
      data_->SeekToFirst();
    } else {
      data_->SeekToLast();
    }
  }
  return {};
}

}  // namespace modern_leveldb
