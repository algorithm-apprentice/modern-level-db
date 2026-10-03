#include "table/block.h"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <string_view>
#include <utility>
#include <vector>

#if MODERN_LEVELDB_READ_DIAGNOSTICS
#include "instrumentation/read_diagnostics.h"
#endif
#include "format/internal_key.h"
#include "modern_leveldb/base/bytes.h"
#include "modern_leveldb/base/coding.h"
#include "modern_leveldb/base/result.h"

namespace modern_leveldb {
namespace {

constexpr std::size_t Fixed32Size = sizeof(std::uint32_t);

struct DecodedEntry {
  std::uint32_t shared;
  ByteView key_delta;
  ByteView value;
};

const std::byte* DecodeVarint32(const std::byte* input, const std::byte* limit,
                                std::uint32_t& value) noexcept {
  value = 0;
  for (unsigned int index = 0; index < 5; ++index) {
    if (input == limit) {
      return nullptr;
    }
    const unsigned int byte = std::to_integer<unsigned int>(*input++);
    value |= static_cast<std::uint32_t>(byte & 0x7fU) << (index * 7U);
    if ((byte & 0x80U) == 0U) {
      return input;
    }
  }
  return nullptr;
}

const std::byte* DecodeEntry(const std::byte* input, const std::byte* limit,
                             DecodedEntry& entry) noexcept {
  if (static_cast<std::size_t>(limit - input) < 3) {
    return nullptr;
  }

  std::uint32_t shared = std::to_integer<unsigned int>(input[0]);
  std::uint32_t non_shared = std::to_integer<unsigned int>(input[1]);
  std::uint32_t value_size = std::to_integer<unsigned int>(input[2]);
  if ((shared | non_shared | value_size) < 128U) {
    input += 3;
  } else {
    if ((input = DecodeVarint32(input, limit, shared)) == nullptr ||
        (input = DecodeVarint32(input, limit, non_shared)) == nullptr ||
        (input = DecodeVarint32(input, limit, value_size)) == nullptr) {
      return nullptr;
    }
  }

  const std::size_t remaining = static_cast<std::size_t>(limit - input);
  if (std::uint64_t{non_shared} + value_size > remaining) {
    return nullptr;
  }
  entry = {
      .shared = shared,
      .key_delta = ByteView(input, non_shared),
      .value = ByteView(input + non_shared, value_size),
  };
  return input;
}

ByteView StringBytes(const std::string& value) noexcept {
  return ByteView(reinterpret_cast<const std::byte*>(value.data()), value.size());
}

}  // namespace

std::size_t Block::Layout::RestartPoint(std::size_t index) const noexcept {
  assert(index < restart_count);
  return DecodeFixed32(contents.subspan(entries_end + Fixed32Size * index).first<Fixed32Size>());
}

Result<Block> Block::Create(std::vector<std::byte> contents) {
  return Create(BlockContents::Owned(std::move(contents)));
}

Result<Block> Block::Create(BlockContents contents) {
  const ByteView data = contents.data();
  if (data.size() < Fixed32Size) {
    return std::unexpected(Error::Corruption("block is too short"));
  }
  const std::size_t restart_count = DecodeFixed32(data.last<Fixed32Size>());
  if (restart_count > (data.size() - Fixed32Size) / Fixed32Size) {
    return std::unexpected(Error::Corruption("block restart count is invalid"));
  }
  const std::size_t entries_end = data.size() - Fixed32Size * (restart_count + 1);
  return Block(std::move(contents), entries_end, restart_count);
}

// GCOVR_EXCL_START: GCC emits duplicate constructor ABI clones
Block::Block(BlockContents contents, std::size_t entries_end, std::size_t restart_count) noexcept
    : contents_(std::move(contents)),
      layout_{
          .contents = contents_.data(),
          .entries_end = entries_end,
          .restart_count = restart_count,
      } {}
// GCOVR_EXCL_STOP

bool Block::empty() const noexcept {
  return layout_.restart_count == 0 || layout_.entries_end == 0 ||
         layout_.RestartPoint(0) >= layout_.entries_end;
}

Status Block::ValidateEntries(const EntryVisitor& visitor) const {
  const Layout& layout = layout_;
  if (layout.restart_count == 0 || layout.RestartPoint(0) != 0) {
    return std::unexpected(Error::Corruption("block restart points are invalid"));
  }

  std::string key;
  std::size_t restart_index = 0;
  for (std::size_t offset = 0; offset < layout.entries_end;) {
#if MODERN_LEVELDB_READ_DIAGNOSTICS
    read_diagnostics::Add(read_diagnostics::Counter::ValidationEntries);
#endif
    DecodedEntry entry{};
    const std::byte* begin = layout.contents.data() + offset;
    const std::byte* limit = layout.contents.data() + layout.entries_end;
    const std::byte* key_delta = DecodeEntry(begin, limit, entry);
    if (key_delta == nullptr || entry.shared > key.size()) {
      return std::unexpected(Error::Corruption("block entry is malformed"));
    }
    const std::size_t entry_end = offset + static_cast<std::size_t>(key_delta - begin) +
                                  entry.key_delta.size() + entry.value.size();

    if (restart_index < layout.restart_count) {
      const std::size_t restart = layout.RestartPoint(restart_index);
      if (restart < offset || (restart == offset && entry.shared != 0)) {
        return std::unexpected(Error::Corruption("block restart points are invalid"));
      }
      if (restart == offset) {
        ++restart_index;
      }
    }

    key.resize(entry.shared);
    key.append(reinterpret_cast<const char*>(entry.key_delta.data()), entry.key_delta.size());
    const Status visited = visitor(StringBytes(key), entry.value);
    if (!visited.has_value()) {
      return visited;
    }
    offset = entry_end;
  }

  const std::size_t matched_restarts = layout.entries_end == 0 ? 1 : restart_index;
  if (matched_restarts != layout.restart_count) {
    return std::unexpected(Error::Corruption("block restart points are invalid"));
  }
  return {};
}

// GCOVR_EXCL_START: GCC emits duplicate constructor ABI clones
Block::Iterator::Iterator(const Block& block, const Comparator& comparator,
                          BlockKeyFormat format) noexcept
    : layout_(block.layout_),
      comparator_(&comparator),
      format_(format),
      current_(layout_.entries_end),
      next_(layout_.entries_end),
      restart_index_(layout_.restart_count) {}
// GCOVR_EXCL_STOP

ByteView Block::Iterator::key() const noexcept {
  assert(valid());
  return StringBytes(key_);
}

ByteView Block::Iterator::value() const noexcept {
  assert(valid());
  return value_;
}

Status Block::Iterator::SeekToFirst() {
  BeginPositioning();
  if (layout_.restart_count == 0) {
    Invalidate();
    return {};
  }
  const Status sought = SeekToRestartPoint(0);
  if (!sought.has_value()) {
    return sought;
  }
  return ParseNextEntry();
}

Status Block::Iterator::SeekToLast() {
  BeginPositioning();
  if (layout_.restart_count == 0) {
    Invalidate();
    return {};
  }
  const Status sought = SeekToRestartPoint(layout_.restart_count - 1);
  if (!sought.has_value()) {
    return sought;
  }
  do {
    const Status parsed = ParseNextEntry();
    if (!parsed.has_value()) {
      return parsed;
    }
  } while (valid() && next_ < layout_.entries_end);
  return {};
}

Status Block::Iterator::Seek(ByteView target) {
  BeginPositioning();
  if (!AcceptsKeySize(target.size())) {
    return Corruption();
  }
  if (layout_.restart_count == 0) {
    Invalidate();
    return {};
  }

  std::size_t left = 0;
  std::size_t right = layout_.restart_count - 1;
  int current_key_compare = 0;
  if (valid()) {
    current_key_compare = Compare(key(), target);
    if (current_key_compare < 0) {
      left = restart_index_;
    } else if (current_key_compare > 0) {
      right = restart_index_;
    } else {
      return {};
    }
  }

  while (left < right) {
    const std::size_t middle = left + (right - left + 1) / 2;
    ByteView middle_key;
    const Status decoded = RestartKey(middle, middle_key);
    if (!decoded.has_value()) {
      return decoded;
    }
    if (Compare(middle_key, target) < 0) {
      left = middle;
    } else {
      right = middle - 1;
    }
  }

  assert(current_key_compare == 0 || valid());
  if (!(left == restart_index_ && current_key_compare < 0)) {
    const Status sought = SeekToRestartPoint(left);
    if (!sought.has_value()) {
      return sought;
    }
  }
  while (true) {
    const Status parsed = ParseNextEntry();
    if (!parsed.has_value() || !valid() || Compare(key(), target) >= 0) {
      return parsed;
    }
  }
}

Status Block::Iterator::Next() {
  assert(valid());
  return ParseNextEntry();
}

Status Block::Iterator::Prev() {
  assert(valid());
  const std::size_t original = current_;
  while (true) {
    std::size_t restart = 0;
    if (!RestartPoint(restart_index_, restart)) {
      return Corruption();
    }
    if (restart < original) {
      break;
    }
    if (restart_index_ == 0) {
      Invalidate();
      return {};
    }
    --restart_index_;
  }

  const Status sought = SeekToRestartPoint(restart_index_);
  // GCOVR_EXCL_START: restart < original proves that this bounded seek succeeds
  if (!sought.has_value()) {
    return sought;
  }
  // GCOVR_EXCL_STOP
  do {
    const Status parsed = ParseNextEntry();
    if (!parsed.has_value()) {
      return parsed;
    }
    assert(valid());
  } while (next_ < original);
  return {};
}

void Block::Iterator::BeginPositioning() noexcept { error_.reset(); }

bool Block::Iterator::RestartPoint(std::size_t index, std::size_t& offset) const noexcept {
  assert(index < layout_.restart_count);
  offset = layout_.RestartPoint(index);
  return offset <= layout_.entries_end;
}

Status Block::Iterator::RestartKey(std::size_t index, ByteView& key) {
  std::size_t offset = 0;
  if (!RestartPoint(index, offset) || offset >= layout_.entries_end) {
    return Corruption();
  }
  DecodedEntry entry{};
  const std::byte* begin = layout_.contents.data() + offset;
  const std::byte* limit = layout_.contents.data() + layout_.entries_end;
  if (DecodeEntry(begin, limit, entry) == nullptr || entry.shared != 0 ||
      !AcceptsKeySize(entry.key_delta.size())) {
    return Corruption();
  }
#if MODERN_LEVELDB_READ_DIAGNOSTICS
  read_diagnostics::RecordDecodedEntry(true);
#endif
  key = entry.key_delta;
  return {};
}

Status Block::Iterator::SeekToRestartPoint(std::size_t index) {
  std::size_t offset = 0;
  if (!RestartPoint(index, offset)) {
    return Corruption();
  }
  current_ = layout_.entries_end;
  restart_index_ = index;
  next_ = offset;
  key_.clear();
  value_ = {};
  return {};
}

Status Block::Iterator::ParseNextEntry() {
  if (next_ >= layout_.entries_end) {
    Invalidate();
    return {};
  }

  DecodedEntry entry{};
  const std::byte* begin = layout_.contents.data() + next_;
  const std::byte* limit = layout_.contents.data() + layout_.entries_end;
  const std::byte* key_delta = DecodeEntry(begin, limit, entry);
  if (key_delta == nullptr || entry.shared > key_.size()) {
    return Corruption();
  }
  const std::size_t key_size = entry.shared + entry.key_delta.size();
  if (!AcceptsKeySize(key_size)) {
    return Corruption();
  }
  const std::size_t entry_end = next_ + static_cast<std::size_t>(key_delta - begin) +
                                entry.key_delta.size() + entry.value.size();

  key_.reserve(key_size);
  current_ = next_;
  key_.resize(entry.shared);
  key_.append(reinterpret_cast<const char*>(entry.key_delta.data()), entry.key_delta.size());
  value_ = entry.value;
  next_ = entry_end;
#if MODERN_LEVELDB_READ_DIAGNOSTICS
  read_diagnostics::RecordDecodedEntry();
#endif
  while (restart_index_ + 1 < layout_.restart_count) {
    std::size_t restart = 0;
    if (!RestartPoint(restart_index_ + 1, restart)) {
      return Corruption();
    }
    if (restart >= current_) {
      break;
    }
    ++restart_index_;
  }
  return {};
}

Status Block::Iterator::Corruption() {
  Invalidate();
  error_.emplace(Error::Corruption("bad entry in block"));
  return std::unexpected(*error_);
}

bool Block::Iterator::AcceptsKeySize(std::size_t size) const noexcept {
  return format_ != BlockKeyFormat::Internal || size >= InternalKeyTrailerSize;
}

int Block::Iterator::Compare(ByteView left, ByteView right) const noexcept {
  return comparator_->Compare(left, right);
}

void Block::Iterator::Invalidate() noexcept {
  current_ = layout_.entries_end;
  next_ = layout_.entries_end;
  restart_index_ = layout_.restart_count;
  key_.clear();
  value_ = {};
}

}  // namespace modern_leveldb
