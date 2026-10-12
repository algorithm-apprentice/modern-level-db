#include "table/block_builder.h"

#include <cstddef>
#include <cstdint>
#include <vector>

#include "base/bytes_internal.h"
#include "modern_leveldb/base/bytes.h"
#include "modern_leveldb/base/coding.h"

namespace modern_leveldb {
namespace {

constexpr std::size_t RestartSize = sizeof(std::uint32_t);

}  // namespace

BlockBuilder::BlockBuilder(std::uint32_t restart_interval) noexcept
    : restart_interval_(restart_interval) {}

void BlockBuilder::Add(ByteView key, ByteView value) {
    const bool restart = entries_since_restart_ == restart_interval_;
    std::size_t shared = 0;
    if (!restart) {
        shared = SharedPrefixLength(last_key_, key);
    }
    const std::size_t non_shared = key.size() - shared;

    if (restart) {
        BeginRestart();
    }
    AppendEntryHeader(shared, non_shared, value.size());
    const ByteView key_delta = key.subspan(shared);
    AppendEntryPayload(key_delta, value);
    CommitEntryHistory(shared, key_delta);
}

void BlockBuilder::BeginRestart() {
    restarts_.push_back(static_cast<std::uint32_t>(buffer_.size()));
    entries_since_restart_ = 0;
}

void BlockBuilder::AppendEntryHeader(std::size_t shared, std::size_t non_shared,
                                     std::size_t value_size) {
    AppendVarint32(buffer_, static_cast<std::uint32_t>(shared));
    AppendVarint32(buffer_, static_cast<std::uint32_t>(non_shared));
    AppendVarint32(buffer_, static_cast<std::uint32_t>(value_size));
}

void BlockBuilder::AppendEntryPayload(ByteView key_delta, ByteView value) {
    buffer_.insert(buffer_.end(), key_delta.begin(), key_delta.end());
    buffer_.insert(buffer_.end(), value.begin(), value.end());
}

void BlockBuilder::CommitEntryHistory(std::size_t shared, ByteView key_delta) {
    last_key_.resize(shared);
    last_key_.insert(last_key_.end(), key_delta.begin(), key_delta.end());
    ++entries_since_restart_;
}

ByteView BlockBuilder::Finish() {
    AppendRestarts();
    AppendFixed32(buffer_, static_cast<std::uint32_t>(restarts_.size() + 1));
    return buffer_;
}

void BlockBuilder::AppendRestarts() {
    AppendFixed32(buffer_, 0);
    for (const std::uint32_t restart : restarts_) {
        AppendFixed32(buffer_, restart);
    }
}

void BlockBuilder::Reset() noexcept {
    buffer_.clear();
    restarts_.clear();
    last_key_.clear();
    entries_since_restart_ = 0;
}

std::size_t BlockBuilder::CurrentSizeEstimate() const noexcept {
    return buffer_.size() + RestartSize * (restarts_.size() + 1) + RestartSize;
}

}  // namespace modern_leveldb
