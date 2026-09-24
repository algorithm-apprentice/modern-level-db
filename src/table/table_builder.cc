#include "table/table_builder.h"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <memory>
#include <utility>
#include <vector>

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
      data_block_(options.restart_interval),
      index_block_(1) {
  if (file_ == nullptr) {
    first_error_ = Error::InvalidArgument("table builder has no file");
  }
  if (options.filter_policy.has_value()) {
    filter_block_.emplace(*options.filter_policy);
    const Status started = filter_block_->StartBlock(0);
    assert(started.has_value());
    (void)started;
    filter_key_.assign("filter.");
    filter_key_.append(options.filter_policy->Name());
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
  if (filter_block_.has_value()) {
    if (!Record(filter_block_->AddKey(key.first(key.size() - InternalKeyTrailerSize)))) {
      return FirstError();
    }
  }

  if (pending_index_entry_) {
    comparator_->FindShortestSeparator(last_key_, key);
    AddPendingIndexEntry();
  }
  last_key_.assign(key.begin(), key.end());
  ++entry_count_;
  Record(data_block_.Add(key, value));
  if (first_error_.has_value()) {  // GCOVR_EXCL_BR_WITHOUT_HIT: 1/2 needs a block over 4 GiB
    return FirstError();           // GCOVR_EXCL_LINE: needs a block over 4 GiB
  }

  if (data_block_.CurrentSizeEstimate() >= block_size_) {
    WriteDataBlock();
    if (first_error_.has_value()) {
      return FirstError();
    }
  }
  return {};
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
  Record(index_block_.Add(last_key_, handle));
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
  handle = BlockHandle{.offset = file_size_, .size = contents.size()};
  const auto trailer = EncodeBlockTrailer(contents);
  // Separate statements keep each Status temporary unconditional; see ADR-0019.
  if (!Record(file_->Append(contents))) {
    return;
  }
  if (Record(file_->Append(trailer))) {
    file_size_ += contents.size() + BlockTrailerSize;
  }
}

void TableBuilder::WriteTail() {
  if (!data_block_.empty()) {
    WriteDataBlock();
  }

  BlockBuilder metaindex(restart_interval_);
  if (filter_block_.has_value()) {
    BlockHandle filter_handle{};
    WriteBlock(filter_block_->Finish(), filter_handle);
    std::vector<std::byte> handle;
    AppendBlockHandle(handle, filter_handle);
    const Status added = metaindex.Add(AsBytes(filter_key_), handle);
    assert(added.has_value());
    (void)added;
  }
  BlockHandle metaindex_handle{};
  WriteBlock(metaindex.Finish(), metaindex_handle);

  if (pending_index_entry_) {
    comparator_->FindShortSuccessor(last_key_);
    AddPendingIndexEntry();
  }
  BlockHandle index_handle{};
  WriteBlock(index_block_.Finish(), index_handle);
  if (first_error_.has_value()) {
    return;
  }

  const auto footer = EncodeFooter({.metaindex = metaindex_handle, .index = index_handle});
  if (Record(file_->Append(footer))) {
    file_size_ += footer.size();
    Record(file_->Sync());
  }
}

}  // namespace modern_leveldb
