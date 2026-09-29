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

#if MODERN_LEVELDB_READ_DIAGNOSTICS
#include "engine/read_diagnostics.h"
#endif
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
  const std::size_t stored_size = static_cast<std::size_t>(handle.size) + BlockTrailerSize;
  if (const std::optional<ByteView> view = file.TryReadView(handle.offset, stored_size);
      view.has_value()) {
#if MODERN_LEVELDB_READ_DIAGNOSTICS
    read_diagnostics::Add(read_diagnostics::Counter::MappedViewBlocks);
    read_diagnostics::Add(read_diagnostics::Counter::MappedViewBytes, view->size());
#endif
    return DecodeStoredBlock(*view);
  }
#if MODERN_LEVELDB_READ_DIAGNOSTICS
  read_diagnostics::Add(read_diagnostics::Counter::CopiedReadBlocks);
  read_diagnostics::Add(read_diagnostics::Counter::CopiedReadBytes, stored_size);
#endif
  std::vector<std::byte> stored(stored_size);
  const Status read = ReadExactly(file, handle.offset, stored);
  if (!read.has_value()) {
    return std::unexpected(read.error());
  }
  return DecodeStoredBlock(std::move(stored));
}

Result<Block> ReadBlock(const RandomAccessFile& file, std::uint64_t blocks_end,
                        BlockHandle handle) {
  Result<std::vector<std::byte>> contents = ReadStoredBlock(file, blocks_end, handle);
  if (!contents.has_value()) {
    return std::unexpected(std::move(contents).error());
  }
#if MODERN_LEVELDB_READ_DIAGNOSTICS
  read_diagnostics::StageScope construction(read_diagnostics::Stage::BlockConstruction);
#endif
  return Block::Create(std::move(*contents));
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
Status ValidateIndex(const Block& index, std::uint64_t blocks_end, bool trusted_internal_keys) {
  std::uint64_t next_offset = 0;
  return index.ValidateEntries(  // GCOVR_EXCL_BR_LINE: GCC 13 closure cleanup
      [&](ByteView key, ByteView encoded_value) -> Status {  // GCOVR_EXCL_BR_LINE
        if (trusted_internal_keys) {
          const Result<ParsedInternalKey> parsed = ParseInternalKey(key);
          if (!parsed.has_value()) {
            return std::unexpected(parsed.error());
          }
        }
        ByteView value = encoded_value;
        const Result<BlockHandle> handle = ConsumeBlockHandle(value);
        if (!handle.has_value() || !value.empty() || !InBlockRegion(*handle, blocks_end)) {
          return std::unexpected(Error::Corruption("table index value is not a block handle"));
        }
        if (handle->offset < next_offset) {
          return std::unexpected(Error::Corruption("table index blocks overlap"));
        }
        next_offset = handle->offset + handle->size + BlockTrailerSize;
        return {};
      });
}

// Reads the filter block that the metaindex maps to the policy, if any.
Status ReadFilter(const RandomAccessFile& file, std::uint64_t blocks_end, BlockHandle metaindex,
                  BloomFilterPolicy policy, std::optional<FilterBlockReader>& filter) {
  const Result<Block> meta = ReadBlock(file, blocks_end, metaindex);
  if (!meta.has_value()) {
    return std::unexpected(meta.error());
  }
  const Status valid = meta->ValidateEntries([](ByteView, ByteView) -> Status { return {}; });
  if (!valid.has_value()) {
    return valid;
  }
  std::string key = "filter.";
  key.append(policy.Name());
  Block::Iterator entry(*meta, BytewiseComparator());
  const Status sought = entry.Seek(AsBytes(key));
  // GCOVR_EXCL_START: full physical validation makes metaindex seek infallible
  if (!sought.has_value()) {
    return sought;
  }
  // GCOVR_EXCL_STOP
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

  Result<Block> index = ReadBlock(*file, blocks_end, footer->index);
  if (!index.has_value()) {
    return std::unexpected(std::move(index).error());
  }
  const Status valid =
      ValidateIndex(*index, blocks_end, options.use_trusted_internal_key_comparison);
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
  // GCOVR_EXCL_START: allocation failure propagates as an exception
  auto table = std::unique_ptr<Table>(new Table(
      std::move(file), blocks_end, comparator, options.use_trusted_internal_key_comparison,
      std::move(*index), std::move(filter), options.block_cache, cache_id));
  // GCOVR_EXCL_STOP
  return table;
}

// GCOVR_EXCL_START: GCC emits duplicate constructor ABI clones
Table::Table(std::unique_ptr<RandomAccessFile> file, std::uint64_t blocks_end,
             const InternalKeyComparator& comparator, bool trusted_internal_keys, Block index,
             std::optional<FilterBlockReader> filter, BlockCache* block_cache,
             std::uint64_t cache_id) noexcept
    : file_(std::move(file)),
      blocks_end_(blocks_end),
      comparator_(&comparator),
      trusted_comparator_(comparator),
      block_comparator_(trusted_internal_keys ? static_cast<const Comparator*>(&trusted_comparator_)
                                              : static_cast<const Comparator*>(comparator_)),
      block_key_format_(trusted_internal_keys ? BlockKeyFormat::Internal
                                              : BlockKeyFormat::Arbitrary),
      index_(std::move(index)),
      filter_(std::move(filter)),
      block_cache_(block_cache),
      cache_id_(cache_id) {}
// GCOVR_EXCL_STOP

Result<std::optional<TableLookup>> Table::Get(const LookupKey& key,
                                              const TableReadOptions& options) const {
  Block::Iterator index(index_, *block_comparator_, block_key_format_);
#if MODERN_LEVELDB_READ_DIAGNOSTICS
  Status index_sought;
  {
    read_diagnostics::BlockRoleScope role(read_diagnostics::BlockRole::Index);
    read_diagnostics::StageScope seek(read_diagnostics::Stage::IndexSeek);
    index_sought = index.Seek(key.internal_key());
  }
#else
  const Status index_sought = index.Seek(key.internal_key());
#endif
  // GCOVR_EXCL_START: full physical index validation makes this seek infallible
  if (!index_sought.has_value()) {
    return std::unexpected(index_sought.error());
  }
  // GCOVR_EXCL_STOP
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
  Block::Iterator entry(block->block(), *block_comparator_, block_key_format_);
#if MODERN_LEVELDB_READ_DIAGNOSTICS
  Status data_sought;
  {
    read_diagnostics::BlockRoleScope role(read_diagnostics::BlockRole::Data);
    read_diagnostics::StageScope seek(read_diagnostics::Stage::DataSeek);
    data_sought = entry.Seek(key.internal_key());
  }
#else
  const Status data_sought = entry.Seek(key.internal_key());
#endif
  if (!data_sought.has_value()) {
    return std::unexpected(data_sought.error());
  }
  if (!entry.valid()) {
    return std::optional<TableLookup>();
  }
  const Result<ParsedInternalKey> parsed = ParseInternalKey(entry.key());
  if (!parsed.has_value()) {
    return std::unexpected(parsed.error());
  }
  if (comparator_->user_comparator().Compare(parsed->user_key, key.user_key()) != 0) {
    return std::optional<TableLookup>();
  }
#if MODERN_LEVELDB_READ_DIAGNOSTICS
  TableLookup lookup;
  lookup.kind = parsed->kind;
  {
    read_diagnostics::StageScope copy(read_diagnostics::Stage::ResultCopy);
    read_diagnostics::Add(read_diagnostics::Counter::ResultBytes, entry.value().size());
    lookup.value.assign(entry.value().begin(), entry.value().end());
  }
#else
  TableLookup lookup{
      .kind = parsed->kind,
      .value = std::vector<std::byte>(entry.value().begin(), entry.value().end()),
  };
#endif
  return lookup;
}

Result<Table::BlockReference> Table::ReadDataBlock(BlockHandle handle,
                                                   const TableReadOptions& options) const {
#if MODERN_LEVELDB_READ_DIAGNOSTICS
  read_diagnostics::BlockRoleScope role(read_diagnostics::BlockRole::Data);
#endif
  std::array<std::byte, 2 * sizeof(std::uint64_t)> cache_key{};
  if (block_cache_ != nullptr) {
    EncodeFixed64(std::span(cache_key).first<sizeof(std::uint64_t)>(), cache_id_);
    EncodeFixed64(std::span(cache_key).last<sizeof(std::uint64_t)>(), handle.offset);
#if MODERN_LEVELDB_READ_DIAGNOSTICS
    std::optional<BlockCache::Handle> cached;
    {
      read_diagnostics::StageScope lookup(read_diagnostics::Stage::BlockCacheLookup);
      cached = block_cache_->Lookup(cache_key);
    }
#else
    std::optional<BlockCache::Handle> cached = block_cache_->Lookup(cache_key);
#endif
    if (cached.has_value()) {
#if MODERN_LEVELDB_READ_DIAGNOSTICS
      read_diagnostics::Add(read_diagnostics::Counter::BlockCacheHits);
#endif
      return BlockReference(std::move(*cached));
    }
#if MODERN_LEVELDB_READ_DIAGNOSTICS
    read_diagnostics::Add(read_diagnostics::Counter::BlockCacheMisses);
#endif
  }

  Result<Block> block = ReadBlock(*file_, blocks_end_, handle);
  if (!block.has_value()) {
    return std::unexpected(std::move(block).error());
  }
  if (block->empty()) {
    return std::unexpected(Error::Corruption("table data block is empty"));
  }
  auto owned = std::make_unique<const Block>(std::move(*block));
  if (block_cache_ != nullptr && options.fill_cache) {
    const std::size_t charge = owned->size();
    Result<BlockCache::Handle> inserted = block_cache_->Insert(cache_key, std::move(owned), charge);
    assert(inserted.has_value());
    return BlockReference(std::move(*inserted));
  }
  return BlockReference(std::move(owned));
}

// GCOVR_EXCL_START: GCC emits duplicate constructor ABI clones
Table::Iterator::Iterator(const Table& table, const TableReadOptions& options) noexcept
    : table_(&table),
      options_(options),
      index_(table.index_, *table.block_comparator_, table.block_key_format_) {}
// GCOVR_EXCL_STOP

ByteView Table::Iterator::key() const noexcept {
  assert(valid());
  return data_->key();
}

ByteView Table::Iterator::value() const noexcept {
  assert(valid());
  return data_->value();
}

Status Table::Iterator::SeekToFirst() {
  const Status sought = index_.SeekToFirst();
  // GCOVR_EXCL_START: full physical index validation makes this move infallible
  if (!sought.has_value()) {
    return Fail(sought.error());
  }
  // GCOVR_EXCL_STOP
  return EnterBlock(Edge::First);
}

Status Table::Iterator::SeekToLast() {
  const Status sought = index_.SeekToLast();
  // GCOVR_EXCL_START: full physical index validation makes this move infallible
  if (!sought.has_value()) {
    return Fail(sought.error());
  }
  // GCOVR_EXCL_STOP
  return EnterBlock(Edge::Last);
}

Status Table::Iterator::Seek(ByteView target) {
  if (table_->block_key_format_ == BlockKeyFormat::Internal) {
    const Result<ParsedInternalKey> parsed = ParseInternalKey(target);
    if (!parsed.has_value()) {
      return Fail(parsed.error());
    }
  }
  const Status index_sought = index_.Seek(target);
  // GCOVR_EXCL_START: validated index bytes and target make this seek infallible
  if (!index_sought.has_value()) {
    return Fail(index_sought.error());
  }
  // GCOVR_EXCL_STOP
  const Status loaded = LoadBlock();
  if (!loaded.has_value()) {
    return Fail(loaded.error());
  }
  if (!data_.has_value()) {
    return {};
  }
  const Status data_sought = data_->Seek(target);
  if (!data_sought.has_value()) {
    return Fail(data_sought.error());
  }
  if (data_->valid()) {
    return ValidatePosition();
  }
  // The target follows the block's last key, and the next block starts after
  // the target.
  const Status advanced = index_.Next();
  // GCOVR_EXCL_START: full physical index validation makes this move infallible
  if (!advanced.has_value()) {
    return Fail(advanced.error());
  }
  // GCOVR_EXCL_STOP
  return EnterBlock(Edge::First);
}

Status Table::Iterator::Next() {
  assert(valid());
  const Status advanced_data = data_->Next();
  if (!advanced_data.has_value()) {
    return Fail(advanced_data.error());
  }
  if (data_->valid()) {
    return ValidatePosition();
  }
  const Status advanced_index = index_.Next();
  // GCOVR_EXCL_START: full physical index validation makes this move infallible
  if (!advanced_index.has_value()) {
    return Fail(advanced_index.error());
  }
  // GCOVR_EXCL_STOP
  return EnterBlock(Edge::First);
}

Status Table::Iterator::Prev() {
  assert(valid());
  const Status retreated_data = data_->Prev();
  if (!retreated_data.has_value()) {
    return Fail(retreated_data.error());
  }
  if (data_->valid()) {
    return ValidatePosition();
  }
  const Status retreated_index = index_.Prev();
  // GCOVR_EXCL_START: full physical index validation makes this move infallible
  if (!retreated_index.has_value()) {
    return Fail(retreated_index.error());
  }
  // GCOVR_EXCL_STOP
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
  data_.emplace(block_->block(), *table_->block_comparator_, table_->block_key_format_);
  return {};
}

Status Table::Iterator::EnterBlock(Edge edge) {
  const Status loaded = LoadBlock();
  if (!loaded.has_value()) {
    return Fail(loaded.error());
  }
  if (data_.has_value()) {
    Status positioned;
    if (edge == Edge::First) {
      positioned = data_->SeekToFirst();
    } else {
      positioned = data_->SeekToLast();
    }
    if (!positioned.has_value()) {
      return Fail(positioned.error());
    }
    if (!data_->valid()) {
      return Fail(Error::Corruption("table data block has no reachable entry"));
    }
    return ValidatePosition();
  }
  return {};
}

Status Table::Iterator::ValidatePosition() {
  assert(data_.has_value() && data_->valid());
  if (table_->block_key_format_ != BlockKeyFormat::Internal) {
    return {};
  }
  const Result<ParsedInternalKey> parsed = ParseInternalKey(data_->key());
  if (!parsed.has_value()) {
    return Fail(parsed.error());
  }
  return {};
}

Status Table::Iterator::Fail(Error error) {
  data_.reset();
  block_.reset();
  return std::unexpected(std::move(error));
}

}  // namespace modern_leveldb
