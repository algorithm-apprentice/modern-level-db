#include "table/block.h"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <utility>
#include <vector>

#include "modern_leveldb/base/bytes.h"
#include "modern_leveldb/base/coding.h"
#include "modern_leveldb/base/comparator.h"
#include "modern_leveldb/base/result.h"

namespace modern_leveldb {
namespace {

constexpr std::size_t Fixed32Size = sizeof(std::uint32_t);

struct Entry {
  std::size_t shared;
  ByteView key_delta;
  ByteView value;
  // Offset just past the entry.
  std::size_t end;
};

// Decodes the entry at offset, which must precede limit. Returns false if the
// entry is malformed or extends past limit.
[[nodiscard]] bool DecodeEntry(ByteView contents, std::size_t offset, std::size_t limit,
                               Entry& entry) {
  ByteView input = contents.subspan(offset, limit - offset);
  const Result<std::uint32_t> shared = ConsumeVarint32(input);
  if (!shared.has_value()) {
    return false;
  }
  const Result<std::uint32_t> non_shared = ConsumeVarint32(input);
  if (!non_shared.has_value()) {
    return false;
  }
  const Result<std::uint32_t> value_size = ConsumeVarint32(input);
  if (!value_size.has_value()) {
    return false;
  }
  if (std::uint64_t{*non_shared} + *value_size > input.size()) {
    return false;
  }
  entry.shared = *shared;
  entry.key_delta = input.first(*non_shared);
  entry.value = input.subspan(*non_shared, *value_size);
  entry.end = limit - input.size() + *non_shared + *value_size;
  return true;
}

}  // namespace

std::size_t Block::Layout::RestartPoint(std::size_t index) const noexcept {
  return DecodeFixed32(contents.subspan(entries_end + Fixed32Size * index).first<Fixed32Size>());
}

ByteView Block::Layout::RestartKey(std::size_t index) const {
  Entry entry{};
  const bool decoded = DecodeEntry(contents, RestartPoint(index), entries_end, entry);
  assert(decoded && entry.shared == 0);
  (void)decoded;
  return entry.key_delta;
}

Result<Block> Block::Create(std::vector<std::byte> contents, const Comparator& comparator) {
  if (contents.size() < Fixed32Size) {
    return std::unexpected(Error::Corruption("block is too short"));
  }
  const std::size_t restart_count = DecodeFixed32(ByteView(contents).last<Fixed32Size>());
  if (restart_count == 0 || restart_count > (contents.size() - Fixed32Size) / Fixed32Size) {
    return std::unexpected(Error::Corruption("block restart count is invalid"));
  }
  const std::size_t entries_end = contents.size() - Fixed32Size * (restart_count + 1);
  Block block(std::move(contents), comparator, entries_end, restart_count);
  Status valid = block.Validate();
  if (!valid.has_value()) {
    return std::unexpected(std::move(valid).error());
  }
  return block;
}

Block::Block(std::vector<std::byte> contents, const Comparator& comparator, std::size_t entries_end,
             std::size_t restart_count) noexcept
    : contents_(std::move(contents)),
      layout_{
          .contents = contents_,
          .comparator = &comparator,
          .entries_end = entries_end,
          .restart_count = restart_count,
      } {}

Status Block::Validate() const {
  const Layout& layout = layout_;
  if (layout.RestartPoint(0) != 0) {
    return std::unexpected(Error::Corruption("block restart points are invalid"));
  }
  std::vector<std::byte> previous_key;
  std::vector<std::byte> key;
  std::size_t restart_index = 0;
  for (std::size_t offset = 0; offset < layout.entries_end;) {
    Entry entry{};
    if (!DecodeEntry(layout.contents, offset, layout.entries_end, entry) ||
        entry.shared > previous_key.size()) {
      return std::unexpected(Error::Corruption("block entry is malformed"));
    }
    if (restart_index < layout.restart_count) {
      const std::size_t restart = layout.RestartPoint(restart_index);
      if (restart < offset || (restart == offset && entry.shared != 0)) {
        return std::unexpected(Error::Corruption("block restart points are invalid"));
      }
      if (restart == offset) {
        ++restart_index;
      }
    }

    const ByteView prefix = ByteView(previous_key).first(entry.shared);
    key.assign(prefix.begin(), prefix.end());
    key.insert(key.end(), entry.key_delta.begin(), entry.key_delta.end());
    if (offset != 0 && layout.comparator->Compare(previous_key, key) >= 0) {
      return std::unexpected(Error::Corruption("block keys are not in increasing order"));
    }
    previous_key.swap(key);
    offset = entry.end;
  }

  // A block without entries has only the restart point at zero. Otherwise,
  // every restart point must be the offset of an entry.
  const std::size_t matched_restarts = layout.entries_end == 0 ? 1 : restart_index;
  if (matched_restarts != layout.restart_count) {
    return std::unexpected(Error::Corruption("block restart points are invalid"));
  }
  return {};
}

Block::Iterator::Iterator(const Block& block) noexcept
    : layout_(block.layout_),
      current_(layout_.entries_end),
      next_(layout_.entries_end),
      restart_index_(layout_.restart_count) {}

ByteView Block::Iterator::key() const noexcept {
  assert(valid());
  return key_;
}

ByteView Block::Iterator::value() const noexcept {
  assert(valid());
  return value_;
}

void Block::Iterator::SeekToFirst() {
  SeekToRestartPoint(0);
  ParseNextEntry();
}

void Block::Iterator::SeekToLast() {
  SeekToRestartPoint(layout_.restart_count - 1);
  while (ParseNextEntry() && next_ < layout_.entries_end) {
  }
}

void Block::Iterator::Seek(ByteView target) {
  // Find the last restart point whose key is less than the target.
  std::size_t left = 0;
  std::size_t right = layout_.restart_count - 1;
  while (left < right) {
    const std::size_t middle = left + (right - left + 1) / 2;
    if (layout_.comparator->Compare(layout_.RestartKey(middle), target) < 0) {
      left = middle;
    } else {
      right = middle - 1;
    }
  }
  SeekToRestartPoint(left);
  while (ParseNextEntry() && layout_.comparator->Compare(key_, target) < 0) {
  }
}

void Block::Iterator::Next() {
  assert(valid());
  ParseNextEntry();
}

void Block::Iterator::Prev() {
  assert(valid());
  const std::size_t original = current_;
  while (layout_.RestartPoint(restart_index_) >= original) {
    if (restart_index_ == 0) {
      Invalidate();
      return;
    }
    --restart_index_;
  }
  SeekToRestartPoint(restart_index_);
  do {
    ParseEntry();
  } while (next_ < original);
}

void Block::Iterator::SeekToRestartPoint(std::size_t index) noexcept {
  current_ = layout_.entries_end;
  restart_index_ = index;
  next_ = layout_.RestartPoint(index);
  key_.clear();
}

void Block::Iterator::ParseEntry() {
  Entry entry{};
  const bool decoded = DecodeEntry(layout_.contents, next_, layout_.entries_end, entry);
  assert(decoded);
  (void)decoded;
  // Reserve first, so that an allocation failure leaves the iterator unchanged.
  key_.reserve(entry.shared + entry.key_delta.size());
  current_ = next_;
  key_.resize(entry.shared);
  key_.insert(key_.end(), entry.key_delta.begin(), entry.key_delta.end());
  value_ = entry.value;
  next_ = entry.end;
  while (restart_index_ + 1 < layout_.restart_count &&
         layout_.RestartPoint(restart_index_ + 1) <= current_) {
    ++restart_index_;
  }
}

bool Block::Iterator::ParseNextEntry() {
  if (next_ >= layout_.entries_end) {
    Invalidate();
    return false;
  }
  ParseEntry();
  return true;
}

void Block::Iterator::Invalidate() noexcept {
  current_ = layout_.entries_end;
  next_ = layout_.entries_end;
  restart_index_ = layout_.restart_count;
  key_.clear();
  value_ = {};
}

}  // namespace modern_leveldb
