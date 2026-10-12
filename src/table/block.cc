#include "table/block.h"

#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <string_view>
#include <utility>
#include <vector>

#include "format/internal_key.h"
#include "modern_leveldb/base/bytes.h"
#include "modern_leveldb/base/coding.h"
#include "modern_leveldb/base/result.h"

namespace modern_leveldb {
namespace {

constexpr std::size_t Fixed32Size = sizeof(std::uint32_t);

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

ByteView StringBytes(const std::string& value) noexcept {
    return ByteView(reinterpret_cast<const std::byte*>(value.data()), value.size());
}

}  // namespace

const std::byte* Block::DecodeEntry(const std::byte* input, const std::byte* limit,
                                    DecodedEntry& entry) noexcept {
    const std::byte* begin = input;
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
        .header_size = static_cast<std::size_t>(input - begin),
    };
    return input;
}

std::optional<Block::DecodedEntry> Block::ReadEntry(const Layout& layout, std::size_t offset,
                                                    std::size_t previous_key_size) noexcept {
    DecodedEntry entry{};
    const std::byte* begin = layout.contents.data() + offset;
    const std::byte* limit = layout.contents.data() + layout.entries_end;
    if (DecodeEntry(begin, limit, entry) == nullptr || entry.shared > previous_key_size) {
        return std::nullopt;
    }
    return entry;
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

    auto restart = FindRestart(target, left, right);
    if (!restart.has_value()) {
        return std::unexpected(std::move(restart).error());
    }
    Status restored = RestoreCheckpoint(*restart, current_key_compare);
    if (!restored.has_value()) {
        return restored;
    }
    return ScanToLowerBound(target);
}

Result<std::size_t> Block::Iterator::FindRestart(ByteView target, std::size_t left,
                                                 std::size_t right) {
    // Prefix-compressed entries require reconstruction from the last restart
    // strictly before the target, rather than a binary search over all entries.
    while (left < right) {
        const std::size_t middle = left + (right - left + 1) / 2;
        ByteView middle_key;
        const Status decoded = RestartKey(middle, middle_key);
        if (!decoded.has_value()) {
            return std::unexpected(decoded.error());
        }
        if (Compare(middle_key, target) < 0) {
            left = middle;
        } else {
            right = middle - 1;
        }
    }
    return left;
}

Status Block::Iterator::RestoreCheckpoint(std::size_t index, int current_key_compare) {
    // Reuse a current forward position only when the checkpoint is unchanged;
    // otherwise rebuild the full key from its restart before applying suffixes.
    if (!(index == restart_index_ && current_key_compare < 0)) {
        const Status sought = SeekToRestartPoint(index);
        if (!sought.has_value()) {
            return sought;
        }
    }
    return {};
}

Status Block::Iterator::ScanToLowerBound(ByteView target) {
    while (true) {
        const Status parsed = ParseNextEntry();
        if (!parsed.has_value() || !valid() || Compare(key(), target) >= 0) {
            return parsed;
        }
    }
}

Status Block::Iterator::Next() { return ParseNextEntry(); }

Status Block::Iterator::Prev() {
    // Entries have no backward link/full key at every offset. Reconstruct from
    // a preceding restart until the next entry would be the current one.
    const std::size_t original = current_;
    bool found = false;
    Status searched = FindPreviousRestart(original, found);
    if (!searched.has_value()) {
        return searched;
    }
    if (!found) {
        return {};
    }

    const Status sought = SeekToRestartPoint(restart_index_);
    // GCOVR_EXCL_START: restart < original proves that this bounded seek succeeds
    if (!sought.has_value()) {
        return sought;
    }
    // GCOVR_EXCL_STOP
    return ScanToPreviousEntry(original);
}

Status Block::Iterator::FindPreviousRestart(std::size_t original, bool& found) {
    while (true) {
        std::size_t restart = 0;
        if (!RestartPoint(restart_index_, restart)) {
            return Corruption();
        }
        if (restart < original) {
            found = true;
            return {};
        }
        if (restart_index_ == 0) {
            Invalidate();
            return {};
        }
        --restart_index_;
    }
}

Status Block::Iterator::ScanToPreviousEntry(std::size_t original) {
    do {
        const Status parsed = ParseNextEntry();
        if (!parsed.has_value()) {
            return parsed;
        }
    } while (next_ < original);
    return {};
}

void Block::Iterator::BeginPositioning() noexcept { error_.reset(); }

bool Block::Iterator::RestartPoint(std::size_t index, std::size_t& offset) const noexcept {
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

    const auto entry = ReadEntry(layout_, next_, key_.size());
    if (!entry.has_value()) {
        return Corruption();
    }
    const std::size_t key_size = entry->shared + entry->key_delta.size();
    if (!AcceptsKeySize(key_size)) {
        return Corruption();
    }
    const std::size_t entry_end =
        next_ + entry->header_size + entry->key_delta.size() + entry->value.size();
    return CommitEntry(*entry, key_size, entry_end);
}

Status Block::Iterator::CommitEntry(const DecodedEntry& entry, std::size_t key_size,
                                    std::size_t entry_end) {
    key_.reserve(key_size);
    current_ = next_;
    ReconstructKey(key_, entry);
    value_ = entry.value;
    next_ = entry_end;
    return AdvanceRestartIndex();
}

Status Block::Iterator::AdvanceRestartIndex() {
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
