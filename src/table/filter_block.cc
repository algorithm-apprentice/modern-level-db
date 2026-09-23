#include "table/filter_block.h"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <utility>
#include <vector>

#include "modern_leveldb/base/bytes.h"
#include "modern_leveldb/base/coding.h"
#include "modern_leveldb/base/result.h"
#include "table/bloom_filter.h"

namespace modern_leveldb {
namespace {

// Each filter covers the data blocks that start in one 2 KiB range.
constexpr unsigned int FilterRangeShift = 11;
constexpr std::size_t Fixed32Size = sizeof(std::uint32_t);
// The array offset and the range shift end the block.
constexpr std::size_t BlockTrailerSize = Fixed32Size + 1;
constexpr std::uint64_t MaximumBlockSize = std::numeric_limits<std::uint32_t>::max();

std::unexpected<Error> BlockTooLarge() {
  return std::unexpected(Error::InvalidArgument("filter block would exceed uint32 size"));
}

}  // namespace

Status FilterBlockBuilder::StartBlock(std::uint64_t block_offset) {
  assert(!finished_);
  const std::uint64_t filter_index = block_offset >> FilterRangeShift;
  assert(filter_index >= filter_offsets_.size());
  if (filter_index == filter_offsets_.size()) {
    return {};
  }

  // Every earlier range gets a filter, starting with the pending keys.
  const std::uint64_t pending_size =
      key_starts_.empty() ? 0 : policy_.FilterSize(key_starts_.size());
  const std::uint64_t finished_size =
      pending_size + result_.size() + Fixed32Size * filter_index + BlockTrailerSize;
  if (finished_size > MaximumBlockSize) {
    return BlockTooLarge();
  }
  while (filter_offsets_.size() < filter_index) {
    GenerateFilter();
  }
  return {};
}

Status FilterBlockBuilder::AddKey(ByteView key) {
  assert(!finished_);
  const std::uint64_t finished_size = std::uint64_t{policy_.FilterSize(key_starts_.size() + 1)} +
                                      result_.size() + Fixed32Size * (filter_offsets_.size() + 1) +
                                      BlockTrailerSize;
  if (finished_size > MaximumBlockSize) {
    return BlockTooLarge();
  }
  key_starts_.push_back(keys_.size());
  keys_.insert(keys_.end(), key.begin(), key.end());
  return {};
}

ByteView FilterBlockBuilder::Finish() {
  assert(!finished_);
  if (!key_starts_.empty()) {
    GenerateFilter();
  }
  const auto array_offset = static_cast<std::uint32_t>(result_.size());
  for (const std::uint32_t offset : filter_offsets_) {
    AppendFixed32(result_, offset);
  }
  AppendFixed32(result_, array_offset);
  result_.push_back(static_cast<std::byte>(FilterRangeShift));
  finished_ = true;
  return result_;
}

void FilterBlockBuilder::GenerateFilter() {
  filter_offsets_.push_back(static_cast<std::uint32_t>(result_.size()));
  if (key_starts_.empty()) {
    return;
  }

  key_starts_.push_back(keys_.size());  // Ends the last key.
  std::vector<ByteView> keys;
  keys.reserve(key_starts_.size() - 1);
  const ByteView flattened = keys_;
  for (std::size_t index = 0; index + 1 < key_starts_.size(); ++index) {
    keys.push_back(
        flattened.subspan(key_starts_[index], key_starts_[index + 1] - key_starts_[index]));
  }
  policy_.CreateFilter(keys, result_);
  keys_.clear();
  key_starts_.clear();
}

Result<FilterBlockReader> FilterBlockReader::Create(std::vector<std::byte> contents,
                                                    BloomFilterPolicy policy) {
  if (contents.size() < BlockTrailerSize) {
    return std::unexpected(Error::Corruption("filter block is too short"));
  }
  if (std::to_integer<unsigned int>(contents.back()) != FilterRangeShift) {
    return std::unexpected(Error::Corruption("filter block has an unsupported range size"));
  }
  const std::size_t offsets_end = contents.size() - BlockTrailerSize;
  const std::size_t array_offset =
      DecodeFixed32(ByteView(contents).subspan(offsets_end).first<Fixed32Size>());
  if (array_offset > offsets_end || (offsets_end - array_offset) % Fixed32Size != 0) {
    return std::unexpected(Error::Corruption("filter block offset array is malformed"));
  }

  FilterBlockReader reader(std::move(contents), policy, array_offset,
                           (offsets_end - array_offset) / Fixed32Size);
  // Filters start at zero, do not decrease, and end at the array offset.
  std::uint32_t previous = 0;
  for (std::size_t index = 0; index <= reader.filter_count_; ++index) {
    const std::uint32_t offset = reader.FilterOffset(index);
    if ((index == 0 && offset != 0) || offset < previous) {
      return std::unexpected(Error::Corruption("filter block offsets are invalid"));
    }
    previous = offset;
  }
  return reader;
}

FilterBlockReader::FilterBlockReader(std::vector<std::byte> contents, BloomFilterPolicy policy,
                                     std::size_t array_offset, std::size_t filter_count) noexcept
    : contents_(std::move(contents)),
      policy_(policy),
      array_offset_(array_offset),
      filter_count_(filter_count) {}

std::uint32_t FilterBlockReader::FilterOffset(std::size_t index) const noexcept {
  return DecodeFixed32(
      ByteView(contents_).subspan(array_offset_ + Fixed32Size * index).first<Fixed32Size>());
}

bool FilterBlockReader::KeyMayMatch(std::uint64_t block_offset, ByteView key) const noexcept {
  const std::uint64_t index = block_offset >> FilterRangeShift;
  if (index >= filter_count_) {
    return true;
  }
  const std::uint32_t start = FilterOffset(static_cast<std::size_t>(index));
  const std::uint32_t limit = FilterOffset(static_cast<std::size_t>(index) + 1);
  return policy_.KeyMayMatch(key, ByteView(contents_).subspan(start, limit - start));
}

}  // namespace modern_leveldb
