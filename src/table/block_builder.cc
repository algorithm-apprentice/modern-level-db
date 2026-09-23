#include "table/block_builder.h"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <vector>

#include "modern_leveldb/base/bytes.h"
#include "modern_leveldb/base/coding.h"
#include "modern_leveldb/base/result.h"

namespace modern_leveldb {
namespace {

constexpr std::uint64_t MaximumBlockSize = std::numeric_limits<std::uint32_t>::max();
constexpr std::size_t RestartSize = sizeof(std::uint32_t);

// GCOVR_EXCL_START: only entries larger than 4 GiB reach this function
std::unexpected<Error> BlockTooLarge() {
  return std::unexpected(Error::InvalidArgument("block would exceed uint32 size"));
}
// GCOVR_EXCL_STOP

}  // namespace

BlockBuilder::BlockBuilder(std::uint32_t restart_interval) noexcept
    : restart_interval_(restart_interval) {
  assert(restart_interval >= 1);
}

Status BlockBuilder::Add(ByteView key, ByteView value) {
  assert(!finished_);
  const bool restart = entries_since_restart_ == restart_interval_;
  std::size_t shared = 0;
  if (!restart) {
    const std::size_t limit = std::min(last_key_.size(), key.size());
    while (shared < limit && last_key_[shared] == key[shared]) {
      ++shared;
    }
  }
  const std::size_t non_shared = key.size() - shared;

  // Sum in 64 bits so that no size can wrap where size_t has 32 bits.
  const std::uint64_t entry_size = std::uint64_t{non_shared} + value.size() + VarintLength(shared) +
                                   VarintLength(non_shared) + VarintLength(value.size());
  const std::uint64_t projected_size =
      entry_size + CurrentSizeEstimate() + (restart ? RestartSize : 0);
  if (projected_size > MaximumBlockSize) {  // GCOVR_EXCL_BR_WITHOUT_HIT: 1/2 needs over 4 GiB
    return BlockTooLarge();                 // GCOVR_EXCL_LINE: needs over 4 GiB
  }

  if (restart) {
    restarts_.push_back(static_cast<std::uint32_t>(buffer_.size()));
    entries_since_restart_ = 0;
  }
  AppendVarint32(buffer_, static_cast<std::uint32_t>(shared));
  AppendVarint32(buffer_, static_cast<std::uint32_t>(non_shared));
  AppendVarint32(buffer_, static_cast<std::uint32_t>(value.size()));
  const ByteView key_delta = key.subspan(shared);
  buffer_.insert(buffer_.end(), key_delta.begin(), key_delta.end());
  buffer_.insert(buffer_.end(), value.begin(), value.end());

  last_key_.resize(shared);
  last_key_.insert(last_key_.end(), key_delta.begin(), key_delta.end());
  ++entries_since_restart_;
  return {};
}

ByteView BlockBuilder::Finish() {
  assert(!finished_);
  AppendFixed32(buffer_, 0);
  for (const std::uint32_t restart : restarts_) {
    AppendFixed32(buffer_, restart);
  }
  AppendFixed32(buffer_, static_cast<std::uint32_t>(restarts_.size() + 1));
  finished_ = true;
  return buffer_;
}

void BlockBuilder::Reset() noexcept {
  buffer_.clear();
  restarts_.clear();
  last_key_.clear();
  entries_since_restart_ = 0;
  finished_ = false;
}

std::size_t BlockBuilder::CurrentSizeEstimate() const noexcept {
  return buffer_.size() + RestartSize * (restarts_.size() + 1) + RestartSize;
}

}  // namespace modern_leveldb
