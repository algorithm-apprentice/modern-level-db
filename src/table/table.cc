#include "table/table.h"

#include <algorithm>
#include <array>
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

#include "base/bytes_internal.h"
#include "base/result_internal.h"
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
#include "table/full_filter.h"

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
            return std::unexpected(
                Error::Io("random-access file returned an oversized table read"));
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
    const std::uint64_t available = std::min<std::uint64_t>(
        blocks_end - handle.offset, std::numeric_limits<std::size_t>::max());
    return available >= BlockTrailerSize && handle.size <= available - BlockTrailerSize;
}

Result<BlockContents> ReadStoredBlock(const RandomAccessFile& file, std::uint64_t blocks_end,
                                      BlockHandle handle) {
    if (!InBlockRegion(handle, blocks_end)) {
        return std::unexpected(Error::Corruption("table block lies outside the file"));
    }
    const std::size_t stored_size = static_cast<std::size_t>(handle.size) + BlockTrailerSize;
    if (const std::optional<ByteView> view = file.TryReadView(handle.offset, stored_size);
        view.has_value()) {
        return DecodeStoredBlock(*view);
    }
    std::vector<std::byte> stored(stored_size);
    const Status read = ReadExactly(file, handle.offset, stored);
    if (!read.has_value()) {
        return std::unexpected(read.error());
    }
    return DecodeStoredBlock(std::move(stored));
}

Result<Block> ReadBlock(const RandomAccessFile& file, std::uint64_t blocks_end,
                        BlockHandle handle) {
    Result<BlockContents> contents = ReadStoredBlock(file, blocks_end, handle);
    if (!contents.has_value()) {
        return std::unexpected(std::move(contents).error());
    }
    return Block::Create(std::move(*contents));
}

// Decodes an index value, which Open validated.
BlockHandle IndexHandle(ByteView value) { return TakeTrusted(ConsumeBlockHandle(value)); }

std::array<std::byte, 2 * sizeof(std::uint64_t)> BlockCacheKey(
    std::uint64_t cache_id, std::uint64_t block_offset) noexcept {
    std::array<std::byte, 2 * sizeof(std::uint64_t)> key{};
    EncodeFixed64(std::span(key).first<sizeof(std::uint64_t)>(), cache_id);
    EncodeFixed64(std::span(key).last<sizeof(std::uint64_t)>(), block_offset);
    return key;
}

bool CanCacheDataBlock(const BlockCache* cache, const TableReadOptions& options,
                       const Block& block) noexcept {
    return cache != nullptr && options.fill_cache && block.cacheable();
}

// Checks that the index values are handles of blocks before the footer that
// follow one another without overlapping, so that an offset identifies a block
// in the block cache.
Status ValidateIndex(const Block& index, std::uint64_t blocks_end,
                     BlockKeyFormat block_key_format) {
    std::uint64_t next_offset = 0;
    const Block::EntryVisitor visitor =
        [&](ByteView key,                        // GCOVR_EXCL_LINE: GCC 13 misses lambda invocation
            ByteView encoded_value) -> Status {  // GCOVR_EXCL_LINE: GCC 13 misses invocation
        if (block_key_format == BlockKeyFormat::Internal) {
            const Result<InternalKeyView> parsed = ParseInternalKey(key);
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
    };
    return index.ValidateEntries(visitor);
}

Result<Footer> ReadFooter(const RandomAccessFile& file, std::uint64_t blocks_end) {
    std::array<std::byte, FooterSize> bytes{};
    const Status read = ReadExactly(file, blocks_end, bytes);
    if (!read.has_value()) {
        return std::unexpected(read.error());
    }
    Result<Footer> footer = DecodeFooter(bytes);
    if (!footer.has_value()) {
        return std::unexpected(footer.error());
    }
    return footer;
}

Result<Block> ReadValidatedIndex(const RandomAccessFile& file, std::uint64_t blocks_end,
                                 BlockHandle handle, BlockKeyFormat key_format) {
    Result<Block> index = ReadBlock(file, blocks_end, handle);
    if (!index.has_value()) {
        return index;
    }
    const Status valid = ValidateIndex(*index, blocks_end, key_format);
    if (!valid.has_value()) {
        return std::unexpected(valid.error());
    }
    return index;
}

Result<Block> ReadValidatedMetaindex(const RandomAccessFile& file, std::uint64_t blocks_end,
                                     BlockHandle handle) {
    Result<Block> meta = ReadBlock(file, blocks_end, handle);
    if (!meta.has_value()) {
        return std::unexpected(meta.error());
    }
    const Status valid = meta->ValidateEntries([](ByteView, ByteView) -> Status { return {}; });
    if (!valid.has_value()) {
        return std::unexpected(valid.error());
    }
    return meta;
}

std::string FilterMetaindexKey(BloomFilterPolicy policy) {
    std::string key = "filter.";
    key.append(policy.Name());
    return key;
}

Result<BlockHandle> DecodeFilterHandle(ByteView value) {
    const Result<BlockHandle> handle = ConsumeBlockHandle(value);
    if (!handle.has_value() || !value.empty()) {
        return std::unexpected(Error::Corruption("table filter handle is malformed"));
    }
    return *handle;
}

Result<std::optional<BlockHandle>> FindFilterHandle(const Block& meta, std::string_view key) {
    Block::Iterator entry(meta, BytewiseComparator());
    const Status sought = entry.Seek(AsBytes(key));
    // GCOVR_EXCL_START: full physical validation makes metaindex seek infallible
    if (!sought.has_value()) {
        return std::unexpected(sought.error());
    }
    // GCOVR_EXCL_STOP
    if (!entry.valid() || AsStringView(entry.key()) != key) {
        return std::nullopt;
    }

    Result<BlockHandle> handle = DecodeFilterHandle(entry.value());
    if (!handle.has_value()) {
        return std::unexpected(std::move(handle).error());
    }
    return std::optional<BlockHandle>(*handle);
}

Result<FilterBlockReader> ReadFilterBlock(const RandomAccessFile& file, std::uint64_t blocks_end,
                                          BlockHandle handle, BloomFilterPolicy policy) {
    Result<BlockContents> contents = ReadStoredBlock(file, blocks_end, handle);
    if (!contents.has_value()) {
        return std::unexpected(std::move(contents).error());
    }
    return FilterBlockReader::Create(std::move(*contents), policy);
}

struct TableFilters {
    std::optional<FilterBlockReader> legacy;
    std::optional<FullFilterReader> full;
};

// Select exactly one recognized layout; unselected metadata is not decoded.
Status ReadFilters(const RandomAccessFile& file, std::uint64_t blocks_end, BlockHandle metaindex,
                   BloomFilterPolicy policy, TableFilters& filters) {
    Result<Block> meta = ReadValidatedMetaindex(file, blocks_end, metaindex);
    if (!meta.has_value()) {
        return std::unexpected(std::move(meta).error());
    }
    Result<std::optional<BlockHandle>> full = FindFilterHandle(*meta, FullFilterMetaKey);
    if (!full.has_value()) {
        return std::unexpected(std::move(full).error());
    }
    if (full->has_value()) {
        if (!IsFullFilterBlockSize((**full).size)) {
            return std::unexpected(Error::Corruption("full filter block size is invalid"));
        }
        Result<BlockContents> contents = ReadStoredBlock(file, blocks_end, **full);
        if (!contents.has_value()) {
            return std::unexpected(std::move(contents).error());
        }
        Result<FullFilterReader> reader = FullFilterReader::Create(std::move(*contents));
        if (!reader.has_value()) {
            return std::unexpected(std::move(reader).error());
        }
        filters.full.emplace(std::move(*reader));
        return {};
    }
    Result<std::optional<BlockHandle>> handle = FindFilterHandle(*meta, FilterMetaindexKey(policy));
    if (!handle.has_value()) {
        return std::unexpected(std::move(handle).error());
    }
    if (!handle->has_value()) {
        return {};
    }
    Result<FilterBlockReader> reader = ReadFilterBlock(file, blocks_end, **handle, policy);
    if (!reader.has_value()) {
        return std::unexpected(std::move(reader).error());
    }
    filters.legacy.emplace(std::move(*reader));
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
    Result<Footer> footer = ReadFooter(*file, blocks_end);
    if (!footer.has_value()) {
        return std::unexpected(std::move(footer).error());
    }

    Result<Block> index =
        ReadValidatedIndex(*file, blocks_end, footer->index, options.block_key_format);
    if (!index.has_value()) {
        return std::unexpected(std::move(index).error());
    }

    TableFilters filters;
    if (options.filter_policy.has_value()) {
        const Status loaded =
            ReadFilters(*file, blocks_end, footer->metaindex, *options.filter_policy, filters);
        if (!loaded.has_value()) {
            return std::unexpected(loaded.error());
        }
    }

    const std::uint64_t cache_id =
        options.block_cache != nullptr ? options.block_cache->NewId() : 0;
    // GCOVR_EXCL_START: allocation failure propagates as an exception
    auto table = std::unique_ptr<Table>(new Table(
        std::move(file), blocks_end, comparator, options.block_key_format, std::move(*index),
        std::move(filters.legacy), std::move(filters.full), options.block_cache, cache_id));
    // GCOVR_EXCL_STOP
    return table;
}

// GCOVR_EXCL_START: GCC emits duplicate constructor ABI clones
Table::Table(std::unique_ptr<RandomAccessFile> file, std::uint64_t blocks_end,
             const InternalKeyComparator& comparator, BlockKeyFormat block_key_format, Block index,
             std::optional<FilterBlockReader> filter, std::optional<FullFilterReader> full_filter,
             BlockCache* block_cache, std::uint64_t cache_id) noexcept
    : file_(std::move(file)),
      blocks_end_(blocks_end),
      comparator_(&comparator),
      block_comparator_(block_key_format == BlockKeyFormat::Internal
                            ? static_cast<const Comparator*>(comparator_)
                            : &BytewiseComparator()),
      block_key_format_(block_key_format),
      index_(std::move(index)),
      filter_(std::move(filter)),
      full_filter_(std::move(full_filter)),
      block_cache_(block_cache),
      cache_id_(cache_id) {}
// GCOVR_EXCL_STOP

Result<TableLookupKind> Table::Get(const LookupKey& key, std::vector<std::byte>& value,
                                   const TableReadOptions& options) const {
    if (full_filter_.has_value() && !full_filter_->KeyMayMatch(key.user_key())) {
        return TableLookupKind::Missing;
    }
    Block::Iterator index(index_, *block_comparator_, block_key_format_);
    const Status index_sought = index.Seek(key.internal_key());
    // GCOVR_EXCL_START: full physical index validation makes this seek infallible
    if (!index_sought.has_value()) {
        return std::unexpected(index_sought.error());
    }
    // GCOVR_EXCL_STOP
    if (!index.valid()) {
        return TableLookupKind::Missing;
    }
    const BlockHandle handle = IndexHandle(index.value());
    if (filter_.has_value() && !filter_->KeyMayMatch(handle.offset, key.user_key())) {
        return TableLookupKind::Missing;
    }

    const Result<BlockReference> block = ReadDataBlock(handle, options);
    if (!block.has_value()) {
        return std::unexpected(block.error());
    }
    return LookupDataBlock(block->block(), key, value);
}

Result<TableLookupKind> Table::LookupDataBlock(const Block& block, const LookupKey& key,
                                               std::vector<std::byte>& value) const {
    Block::Iterator entry(block, *block_comparator_, block_key_format_);
    const Status data_sought = entry.Seek(key.internal_key());
    if (!data_sought.has_value()) {
        return std::unexpected(data_sought.error());
    }
    if (!entry.valid()) {
        return TableLookupKind::Missing;
    }
    const Result<InternalKeyView> parsed = ParseInternalKey(entry.key());
    if (!parsed.has_value()) {
        return std::unexpected(parsed.error());
    }
    if (comparator_->user_comparator().Compare(parsed->user_key, key.user_key()) != 0) {
        return TableLookupKind::Missing;
    }
    if (parsed->kind == ValueKind::Deletion) {
        return TableLookupKind::Deletion;
    }
    CopyLookupValue(entry.value(), value);
    return TableLookupKind::Value;
}

Result<Table::BlockReference> Table::ReadDataBlock(BlockHandle handle,
                                                   const TableReadOptions& options) const {
    std::array<std::byte, 2 * sizeof(std::uint64_t)> cache_key{};
    if (block_cache_ != nullptr) {
        cache_key = BlockCacheKey(cache_id_, handle.offset);
        std::optional<BlockCache::Handle> cached = block_cache_->Lookup(cache_key);
        if (cached.has_value()) {
            return BlockReference(std::move(*cached));
        }
    }

    Result<Block> block = ReadBlock(*file_, blocks_end_, handle);
    if (!block.has_value()) {
        return std::unexpected(std::move(block).error());
    }
    if (block->empty()) {
        return std::unexpected(Error::Corruption("table data block is empty"));
    }
    auto owned = std::make_unique<const Block>(std::move(*block));
    if (CanCacheDataBlock(block_cache_, options, *owned)) {
        const std::size_t charge = owned->size();
        return BlockReference(
            TakeTrusted(block_cache_->Insert(cache_key, std::move(owned), charge)));
    }
    return BlockReference(std::move(owned));
}

// GCOVR_EXCL_START: GCC emits duplicate constructor ABI clones
Table::Iterator::Iterator(const Table& table, const TableReadOptions& options) noexcept
    : table_(&table),
      options_(options),
      index_(table.index_, *table.block_comparator_, table.block_key_format_) {}
// GCOVR_EXCL_STOP

ByteView Table::Iterator::key() const noexcept { return data_->key(); }

ByteView Table::Iterator::value() const noexcept { return data_->value(); }

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
        const Result<InternalKeyView> parsed = ParseInternalKey(target);
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
        return {};
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
    const Status advanced_data = data_->Next();
    if (!advanced_data.has_value()) {
        return Fail(advanced_data.error());
    }
    if (data_->valid()) {
        return {};
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
    const Status retreated_data = data_->Prev();
    if (!retreated_data.has_value()) {
        return Fail(retreated_data.error());
    }
    if (data_->valid()) {
        return {};
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
    if (!index_.valid()) {
        data_.reset();
        block_.reset();
        return {};
    }
    const ByteView handle = index_.value();
    if (data_.has_value() && std::ranges::equal(handle, block_handle_)) {
        return {};
    }
    data_.reset();
    block_.reset();
    Result<BlockReference> block = table_->ReadDataBlock(IndexHandle(handle), options_);
    if (!block.has_value()) {
        return std::unexpected(std::move(block).error());
    }
    block_.emplace(std::move(*block));
    data_.emplace(block_->block(), *table_->block_comparator_, table_->block_key_format_);
    block_handle_ = handle;
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
        return {};
    }
    return {};
}

Status Table::Iterator::Fail(Error error) {
    data_.reset();
    block_.reset();
    return std::unexpected(std::move(error));
}

}  // namespace modern_leveldb
