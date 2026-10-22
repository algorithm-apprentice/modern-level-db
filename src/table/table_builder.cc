#include "table/table_builder.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <utility>
#include <vector>

#include "base/result_internal.h"
#include "format/internal_key.h"
#include "modern_leveldb/base/bytes.h"
#include "modern_leveldb/base/result.h"
#include "platform/file_system.h"
#include "table/block_builder.h"
#include "table/block_format.h"

namespace modern_leveldb {

TableBuilder::TableBuilder(std::unique_ptr<WritableFile> file,
                           const InternalKeyComparator& comparator,
                           const TableBuilderOptions& options)
    : file_(std::move(file)),
      comparator_(&comparator),
      block_size_(options.block_size),
      restart_interval_(options.restart_interval),
      compression_(options.compression),
      zstd_compression_level_(options.zstd_compression_level),
      data_block_(options.restart_interval),
      index_block_(1) {
    if (file_ == nullptr) {
        first_error_ = Error::InvalidArgument("table builder has no file");
    }
    Record(ValidateCompressionOptions(compression_, zstd_compression_level_));
    if (options.filter_layout != FilterLayout::BlockBased &&
        options.filter_layout != FilterLayout::FullTable) {
        Record(std::unexpected(Error::InvalidArgument("table filter layout is invalid")));
        return;
    }
    if (options.filter_policy.has_value()) {
        if (options.filter_layout == FilterLayout::FullTable) {
            full_filter_.emplace(*options.filter_policy);
            filter_key_.assign(FullFilterMetaKey);
        } else {
            filter_block_.emplace(*options.filter_policy);
            static_cast<void>(filter_block_->StartBlock(0));
            filter_key_.assign("filter.");
            filter_key_.append(options.filter_policy->Name());
        }
    }
}

Status TableBuilder::Add(ByteView key, ByteView value) {
    if (finished_) {
        return std::unexpected(Error::InvalidArgument("table builder is finished"));
    }
    if (first_error_.has_value()) {
        return FirstError();
    }
    if (!ParseInternalKey(key).has_value()) {
        Record(std::unexpected(Error::InvalidArgument("table key is not an internal key")));
        return FirstError();
    }
    if (entry_count_ > 0 && comparator_->Compare(key, last_key_) <= 0) {
        Record(std::unexpected(Error::InvalidArgument("table keys must strictly increase")));
        return FirstError();
    }
    return AddValid(key, value);
}

Status TableBuilder::AddTrusted(ByteView key, ByteView value) {
    if (finished_) {
        return std::unexpected(Error::InvalidArgument("table builder is finished"));
    }
    if (first_error_.has_value()) {
        return FirstError();
    }
    return AddValid(key, value);
}

Status TableBuilder::AddValid(ByteView key, ByteView value) {
    if (!AddFilterKey(key)) {
        return FirstError();
    }

    FinishPendingIndexEntry(key);
    AppendDataEntry(key, value);
    if (!FlushDataBlockIfNeeded()) {
        return FirstError();
    }
    return {};
}

bool TableBuilder::AddFilterKey(ByteView key) {
    if (full_filter_.has_value()) {
        return Record(full_filter_->AddKey(key.first(key.size() - InternalKeyTrailerSize)));
    }
    if (!filter_block_.has_value()) {
        return true;
    }
    return Record(filter_block_->AddKey(key.first(key.size() - InternalKeyTrailerSize)));
}

void TableBuilder::FinishPendingIndexEntry(ByteView next_key) {
    if (pending_index_entry_) {
        comparator_->FindShortestSeparator(last_key_, next_key);
        AddPendingIndexEntry();
    }
}

void TableBuilder::AppendDataEntry(ByteView key, ByteView value) {
    last_key_.assign(key.begin(), key.end());
    ++entry_count_;
    data_block_.Add(key, value);
}

bool TableBuilder::FlushDataBlockIfNeeded() {
    if (data_block_.CurrentSizeEstimate() >= block_size_) {
        WriteDataBlock();
    }
    return !first_error_.has_value();
}

Status TableBuilder::Finish() {
    if (finished_) {
        return std::unexpected(Error::InvalidArgument("table builder is finished"));
    }
    finished_ = true;
    if (!first_error_.has_value()) {
        WriteTail();
    }
    if (file_ != nullptr) {
        Record(file_->Close());
    }
    if (first_error_.has_value()) {
        return FirstError();
    }
    return {};
}

bool TableBuilder::Record(Status status) {
    if (status.has_value()) {
        return true;
    }
    if (!first_error_.has_value()) {
        first_error_ = std::move(status).error();
    }
    return false;
}

Status TableBuilder::FirstError() const { return std::unexpected(*first_error_); }

void TableBuilder::AddPendingIndexEntry() {
    std::vector<std::byte> handle;
    AppendBlockHandle(handle, pending_handle_);
    index_block_.Add(last_key_, handle);
    pending_index_entry_ = false;
}

void TableBuilder::WriteDataBlock() {
    WriteBlock(data_block_.Finish(), pending_handle_);
    data_block_.Reset();
    pending_index_entry_ = true;
    if (filter_block_.has_value()) {
        Record(filter_block_->StartBlock(file_size_));
    }
}

void TableBuilder::WriteBlock(ByteView contents, BlockHandle& handle) {
    if (first_error_.has_value()) {
        return;
    }
    const bool compressed =
        TryCompressBlock(contents, compression_, zstd_compression_level_, compressed_output_);
    const ByteView stored = compressed ? ByteView(compressed_output_) : contents;
    const BlockCompression type = compressed ? compression_ : BlockCompression::None;
    WriteRawBlock(stored, type, handle);
    compressed_output_.clear();
}

void TableBuilder::WriteRawBlock(ByteView contents, BlockCompression type, BlockHandle& handle) {
    if (first_error_.has_value()) {
        return;
    }
    handle = BlockHandle{.offset = file_size_, .size = contents.size()};
    const auto trailer = EncodeBlockTrailer(contents, type);
    // Separate statements keep each Status temporary unconditional; see ADR-0019.
    if (!Record(file_->Append(contents))) {
        return;
    }
    if (Record(file_->Append(trailer))) {
        file_size_ += contents.size() + BlockTrailerSize;
    }
}

void TableBuilder::AddFilterIndexEntry(BlockBuilder& metaindex) {
    if (filter_block_.has_value() || full_filter_.has_value()) {
        BlockHandle filter_handle{};
        const ByteView payload =
            full_filter_.has_value() ? full_filter_->Finish() : filter_block_->Finish();
        WriteRawBlock(payload, BlockCompression::None, filter_handle);
        std::vector<std::byte> handle;
        AppendBlockHandle(handle, filter_handle);
        metaindex.Add(AsBytes(filter_key_), handle);
    }
}

BlockHandle TableBuilder::WriteMetaindex() {
    BlockBuilder metaindex(restart_interval_);
    AddFilterIndexEntry(metaindex);
    BlockHandle handle{};
    WriteBlock(metaindex.Finish(), handle);
    return handle;
}

void TableBuilder::AddFinalIndexEntry() {
    if (pending_index_entry_) {
        comparator_->FindShortSuccessor(last_key_);
        AddPendingIndexEntry();
    }
}

BlockHandle TableBuilder::WriteIndex() {
    AddFinalIndexEntry();
    BlockHandle handle{};
    WriteBlock(index_block_.Finish(), handle);
    return handle;
}

void TableBuilder::WriteFooter(BlockHandle metaindex_handle, BlockHandle index_handle) {
    const auto footer = EncodeFooter({.metaindex = metaindex_handle, .index = index_handle});
    if (Record(file_->Append(footer))) {
        file_size_ += footer.size();
        Record(file_->Sync());
    }
}

void TableBuilder::WriteTail() {
    if (!data_block_.empty()) {
        WriteDataBlock();
    }
    const BlockHandle metaindex_handle = WriteMetaindex();
    const BlockHandle index_handle = WriteIndex();
    if (first_error_.has_value()) {
        return;
    }
    WriteFooter(metaindex_handle, index_handle);
}

}  // namespace modern_leveldb
