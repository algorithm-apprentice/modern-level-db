#include "table/block.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <source_location>
#include <string_view>
#include <utility>
#include <vector>

#include "base/coding_internal.h"
#include "format/internal_key.h"
#include "modern_leveldb/base/bytes.h"
#include "modern_leveldb/base/coding.h"
#include "modern_leveldb/base/result.h"

namespace modern_leveldb {
namespace {

constexpr std::size_t Fixed32Size = sizeof(std::uint32_t);

const std::byte* DecodeVarint32Fallback(const std::byte* input, const std::byte* limit,
                                        std::uint32_t& value) noexcept {
    value = 0;
    for (unsigned int index = 0; index < 5; ++index) {
        if (input == limit) {
            return nullptr;
        }
        const unsigned int byte = std::to_integer<unsigned int>(*input++);
        FoldVarintPayload(value, byte & 0x7fU, index * 7U);
        if ((byte & 0x80U) == 0U) {
            return input;
        }
    }
    return nullptr;
}

const std::byte* DecodeVarint32(const std::byte* input, const std::byte* limit,
                                std::uint32_t& value) noexcept {
    if (input != limit && std::to_integer<unsigned int>(*input) < 128U) {
        value = std::to_integer<unsigned int>(*input);
        return input + 1;
    }
    return DecodeVarint32Fallback(input, limit, value);
}

bool DecodeEntryHeader(const std::byte*& input, const std::byte* limit, std::uint32_t& shared,
                       std::uint32_t& non_shared, std::uint32_t& value_size) noexcept {
    if ((shared | non_shared | value_size) < 128U) {
        input += 3;
        return true;
    }
    input = DecodeVarint32(input, limit, shared);
    if (input == nullptr) {
        return false;
    }
    input = DecodeVarint32(input, limit, non_shared);
    if (input == nullptr) {
        return false;
    }
    input = DecodeVarint32(input, limit, value_size);
    return input != nullptr;
}

bool HasEntryPayload(const std::byte* input, const std::byte* limit, std::uint32_t non_shared,
                     std::uint32_t value_size) noexcept {
    const std::size_t remaining = static_cast<std::size_t>(limit - input);
    return std::uint64_t{non_shared} + value_size <= remaining;
}

Result<std::size_t> ReadRestartCount(
    ByteView data, std::source_location location = std::source_location::current()) {
    if (data.size() < Fixed32Size) {
        return std::unexpected(Error::Corruption("block is too short", location));
    }
    const std::size_t restart_count = DecodeFixed32(data.last<Fixed32Size>());
    if (restart_count > (data.size() - Fixed32Size) / Fixed32Size) {
        return std::unexpected(Error::Corruption("block restart count is invalid", location));
    }
    return restart_count;
}

ByteView StringBytes(const std::string& value) noexcept {
    return ByteView(reinterpret_cast<const std::byte*>(value.data()), value.size());
}

}  // namespace

const std::byte* Block::DecodeEntry(const std::byte* input, const std::byte* limit,
                                    std::uint32_t& shared, std::uint32_t& non_shared,
                                    std::uint32_t& value_size) noexcept {
    if (static_cast<std::size_t>(limit - input) < 3) {
        return nullptr;
    }

    shared = std::to_integer<unsigned int>(input[0]);
    non_shared = std::to_integer<unsigned int>(input[1]);
    value_size = std::to_integer<unsigned int>(input[2]);
    if (!DecodeEntryHeader(input, limit, shared, non_shared, value_size)) {
        return nullptr;
    }
    if (!HasEntryPayload(input, limit, non_shared, value_size)) {
        return nullptr;
    }
    return input;
}

std::optional<Block::DecodedEntry> Block::ReadEntry(const Layout& layout, std::size_t offset,
                                                    std::size_t previous_key_size) noexcept {
    const std::byte* begin = layout.contents.data() + offset;
    const std::byte* limit = layout.contents.data() + layout.entries_end;
    std::uint32_t shared = 0;
    std::uint32_t non_shared = 0;
    std::uint32_t value_size = 0;
    const std::byte* key = DecodeEntry(begin, limit, shared, non_shared, value_size);
    if (key == nullptr || shared > previous_key_size) {
        return std::nullopt;
    }
    return DecodedEntry{
        .shared = shared,
        .key_delta = ByteView(key, non_shared),
        .value = ByteView(key + non_shared, value_size),
        .header_size = static_cast<std::size_t>(key - begin),
    };
}

void Block::ReconstructKey(std::string& key, const DecodedEntry& entry) {
    key.resize(entry.shared);
    key.append(reinterpret_cast<const char*>(entry.key_delta.data()), entry.key_delta.size());
}

Status Block::ValidateRestartEntry(const Layout& layout, std::size_t offset, std::uint32_t shared,
                                   std::size_t& restart_index) {
    if (restart_index < layout.restart_count) {
        const std::size_t restart = layout.RestartPoint(restart_index);
        if (restart < offset || (restart == offset && shared != 0)) {
            return std::unexpected(Error::Corruption("block restart points are invalid"));
        }
        if (restart == offset) {
            ++restart_index;
        }
    }
    return {};
}

std::size_t Block::Layout::RestartPoint(std::size_t index) const noexcept {
    return DecodeFixed32(contents.subspan(entries_end + Fixed32Size * index).first<Fixed32Size>());
}

Result<Block> Block::Create(std::vector<std::byte> contents) {
    return Create(BlockContents::Owned(std::move(contents)));
}

Result<Block> Block::Create(BlockContents contents) {
    const ByteView data = contents.data();
    Result<std::size_t> restart_count = ReadRestartCount(data);
    if (!restart_count.has_value()) {
        return std::unexpected(std::move(restart_count).error());
    }
    const std::size_t entries_end = data.size() - Fixed32Size * (*restart_count + 1);
    return Block(std::move(contents), entries_end, *restart_count);
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
        const auto entry = ReadEntry(layout, offset, key.size());
        if (!entry.has_value()) {
            return std::unexpected(Error::Corruption("block entry is malformed"));
        }
        const std::size_t entry_end =
            offset + entry->header_size + entry->key_delta.size() + entry->value.size();
        Status matched = ValidateRestartEntry(layout, offset, entry->shared, restart_index);
        if (!matched.has_value()) {
            return matched;
        }

        ReconstructKey(key, *entry);
        const Status visited = visitor(StringBytes(key), entry->value);
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

ByteView Block::Iterator::key() const noexcept { return StringBytes(key_); }

ByteView Block::Iterator::value() const noexcept { return value_; }

Status Block::Iterator::SeekToFirst() {
    BeginPositioning();
    if (layout_.restart_count == 0) {
        Invalidate();
        return {};
    }
    if (SeekToRestartPoint(0)) {
        ParseNextEntry();
    }
    return CurrentStatus();
}

Status Block::Iterator::SeekToLast() {
    BeginPositioning();
    if (layout_.restart_count == 0) {
        Invalidate();
        return {};
    }
    if (SeekToRestartPoint(layout_.restart_count - 1)) {
        ScanToLastEntry();
    }
    return CurrentStatus();
}

void Block::Iterator::ScanToLastEntry() {
    while (ParseNextEntry() && next_ < layout_.entries_end) {
    }
}

Status Block::Iterator::Seek(ByteView target) {
    BeginPositioning();
    if (!AcceptsKeySize(target.size())) {
        Corruption();
        return CurrentStatus();
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

    std::size_t restart = 0;
    if (!FindRestart(target, left, right, restart) ||
        !RestoreCheckpoint(restart, current_key_compare)) {
        return CurrentStatus();
    }
    ScanToLowerBound(target);
    return CurrentStatus();
}

bool Block::Iterator::FindRestart(ByteView target, std::size_t left, std::size_t right,
                                  std::size_t& index) noexcept {
    // Prefix-compressed entries require reconstruction from the last restart
    // strictly before the target, rather than a binary search over all entries.
    while (left < right) {
        const std::size_t middle = left + (right - left + 1) / 2;
        ByteView middle_key;
        if (!RestartKey(middle, middle_key)) {
            return false;
        }
        if (Compare(middle_key, target) < 0) {
            left = middle;
        } else {
            right = middle - 1;
        }
    }
    index = left;
    return true;
}

bool Block::Iterator::RestoreCheckpoint(std::size_t index, int current_key_compare) noexcept {
    // Reuse a current forward position only when the checkpoint is unchanged;
    // otherwise rebuild the full key from its restart before applying suffixes.
    if (!(index == restart_index_ && current_key_compare < 0)) {
        return SeekToRestartPoint(index);
    }
    return true;
}

void Block::Iterator::ScanToLowerBound(ByteView target) {
    while (ParseNextEntry() && Compare(key(), target) < 0) {
    }
}

Status Block::Iterator::Next() {
    ParseNextEntry();
    return CurrentStatus();
}

Status Block::Iterator::Prev() {
    // Entries have no backward link/full key at every offset. Reconstruct from
    // a preceding restart until the next entry would be the current one.
    const std::size_t original = current_;
    if (!FindPreviousRestart(original)) {
        return CurrentStatus();
    }

    const bool sought = SeekToRestartPoint(restart_index_);
    // GCOVR_EXCL_START: restart < original proves that this bounded seek succeeds
    if (!sought) {
        return CurrentStatus();
    }
    // GCOVR_EXCL_STOP
    ScanToPreviousEntry(original);
    return CurrentStatus();
}

bool Block::Iterator::FindPreviousRestart(std::size_t original) noexcept {
    while (true) {
        std::size_t restart = 0;
        if (!RestartPoint(restart_index_, restart)) {
            return Corruption();
        }
        if (restart < original) {
            return true;
        }
        if (restart_index_ == 0) {
            Invalidate();
            return false;
        }
        --restart_index_;
    }
}

void Block::Iterator::ScanToPreviousEntry(std::size_t original) {
    while (ParseNextEntry() && next_ < original) {
    }
}

void Block::Iterator::BeginPositioning() noexcept { corrupted_ = false; }

bool Block::Iterator::RestartPoint(std::size_t index, std::size_t& offset) const noexcept {
    offset = layout_.RestartPoint(index);
    return offset <= layout_.entries_end;
}

bool Block::Iterator::RestartKey(std::size_t index, ByteView& key) noexcept {
    std::size_t offset = 0;
    if (!RestartPoint(index, offset) || offset >= layout_.entries_end) {
        return Corruption();
    }
    const std::byte* begin = layout_.contents.data() + offset;
    const std::byte* limit = layout_.contents.data() + layout_.entries_end;
    std::uint32_t shared = 0;
    std::uint32_t non_shared = 0;
    std::uint32_t value_size = 0;
    const std::byte* decoded = DecodeEntry(begin, limit, shared, non_shared, value_size);
    if (decoded == nullptr || shared != 0 || !AcceptsKeySize(non_shared)) {
        return Corruption();
    }
    key = ByteView(decoded, non_shared);
    return true;
}

bool Block::Iterator::SeekToRestartPoint(std::size_t index) noexcept {
    std::size_t offset = 0;
    if (!RestartPoint(index, offset)) {
        return Corruption();
    }
    current_ = layout_.entries_end;
    restart_index_ = index;
    next_ = offset;
    key_.clear();
    value_ = {};
    return true;
}

bool Block::Iterator::ParseNextEntry() {
    if (next_ >= layout_.entries_end) {
        Invalidate();
        return false;
    }

    const std::byte* begin = layout_.contents.data() + next_;
    const std::byte* limit = layout_.contents.data() + layout_.entries_end;
    std::uint32_t shared = 0;
    std::uint32_t non_shared = 0;
    std::uint32_t value_size = 0;
    const std::byte* key = DecodeEntry(begin, limit, shared, non_shared, value_size);
    if (key == nullptr || shared > key_.size()) {
        return Corruption();
    }
    const std::size_t key_size = std::size_t{shared} + non_shared;
    if (!AcceptsKeySize(key_size)) {
        return Corruption();
    }
    return CommitEntry(key, shared, non_shared, value_size);
}

bool Block::Iterator::CommitEntry(const std::byte* key, std::uint32_t shared,
                                  std::uint32_t non_shared, std::uint32_t value_size) {
    key_.reserve(std::size_t{shared} + non_shared);
    current_ = next_;
    key_.resize(shared);
    key_.append(reinterpret_cast<const char*>(key), non_shared);
    value_ = ByteView(key + non_shared, value_size);
    next_ = static_cast<std::size_t>(value_.data() + value_.size() - layout_.contents.data());
    return AdvanceRestartIndex();
}

bool Block::Iterator::AdvanceRestartIndex() noexcept {
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
    return true;
}

bool Block::Iterator::Corruption() noexcept {
    Invalidate();
    corrupted_ = true;
    return false;
}

Status Block::Iterator::CurrentStatus() const {
    if (corrupted_) {
        return std::unexpected(Error::Corruption("bad entry in block"));
    }
    return {};
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
