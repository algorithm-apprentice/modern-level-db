#include "memory/memtable.h"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "base/coding_internal.h"
#include "format/internal_key.h"
#include "modern_leveldb/base/bytes.h"
#include "modern_leveldb/base/coding.h"
#include "modern_leveldb/base/comparator.h"
#include "modern_leveldb/base/result.h"

namespace modern_leveldb {
namespace {

struct EntryView {
    ByteView internal_key;
    ByteView value;
};

ByteView DecodeInternalKey(const std::byte* entry) noexcept {
    return ConsumeLengthPrefixedTrusted(entry);
}

EntryView DecodeEntry(const std::byte* entry) noexcept {
    const ByteView internal_key = ConsumeLengthPrefixedTrusted(entry);
    const ByteView value = ConsumeLengthPrefixedTrusted(entry);
    return {
        .internal_key = internal_key,
        .value = value,
    };
}

std::size_t TrustedEncodedSize(ByteView key, ByteView value) noexcept {
    const std::size_t internal_key_size = key.size() + InternalKeyTrailerSize;
    return VarintLength(static_cast<std::uint32_t>(internal_key_size)) + internal_key_size +
           VarintLength(static_cast<std::uint32_t>(value.size())) + value.size();
}

const std::byte* EncodeEntry(Arena& arena, SequenceNumber sequence, ValueKind kind, ByteView key,
                             ByteView value, std::size_t encoded_size) {
    MutableByteView output = arena.Allocate(encoded_size);
    const std::byte* const entry = output.data();
    const std::size_t internal_key_size = key.size() + InternalKeyTrailerSize;
    EncodeVarint32Trusted(output, static_cast<std::uint32_t>(internal_key_size));

    std::ranges::copy(key, output.begin());
    output = output.subspan(key.size());
    EncodeFixed64(
        std::span<std::byte, InternalKeyTrailerSize>(output.data(), InternalKeyTrailerSize),
        PackInternalKeyTrailer(sequence, kind));
    output = output.subspan(InternalKeyTrailerSize);

    EncodeLengthPrefixedTrusted(output, value);
    assert(output.empty());
    return entry;
}

}  // namespace

// GCOVR_EXCL_START: GCC emits duplicate constructor/destructor ABI clones
MemTable::ReadPin::ReadPin(const MemTable& table) noexcept : table_(&table) {
    ++table_->read_pins_;
}

MemTable::ReadPin::ReadPin(ReadPin&& source) noexcept : table_(source.table_) {
    source.table_ = nullptr;
}

MemTable::ReadPin::~ReadPin() {
    if (table_ != nullptr) {
        assert(table_->read_pins_ > 0);
        --table_->read_pins_;
    }
}
// GCOVR_EXCL_STOP

const MemTable& MemTable::ReadPin::value() const noexcept {
    assert(table_ != nullptr);
    return *table_;
}

MemTable::MemTable(const Comparator& user_comparator)
    : user_comparator_(user_comparator),
      internal_comparator_(user_comparator),
      entry_comparator_{internal_comparator_},
      table_(entry_comparator_, arena_) {}

Status MemTable::Add(SequenceNumber sequence, ValueKind kind, ByteView key, ByteView value) {
    if (sequence > MaxSequenceNumber) {
        return std::unexpected(Error::InvalidArgument("memtable sequence exceeds 56 bits"));
    }
    if (!IsValidValueKind(kind)) {
        return std::unexpected(Error::InvalidArgument("memtable value kind is unsupported"));
    }

    const std::byte* const entry =
        EncodeEntry(arena_, sequence, kind, key, value, TrustedEncodedSize(key, value));
    if (!table_.Insert(entry)) {
        return std::unexpected(Error::InvalidArgument("memtable internal key already exists"));
    }
    return {};
}

void MemTable::AddTrusted(SequenceNumber sequence, ValueKind kind, ByteView key, ByteView value) {
    assert(sequence <= MaxSequenceNumber);
    assert(IsValidValueKind(kind));
    const std::byte* const entry =
        EncodeEntry(arena_, sequence, kind, key, value, TrustedEncodedSize(key, value));
    table_.InsertTrusted(entry);
}

MemTableLookup MemTable::Lookup(const LookupKey& key) const {
    // Descending sequence order skips versions newer than the lookup's sequence.
    // A lower-bound result still needs equality on the user-key portion.
    Table::Iterator iterator(table_);
    iterator.Seek(key.memtable_key().data());
    if (!iterator.valid()) {
        return {};
    }

    const EntryView entry = DecodeEntry(iterator.entry());
    const ByteView candidate_user_key =
        entry.internal_key.first(entry.internal_key.size() - InternalKeyTrailerSize);
    if (user_comparator_.Compare(candidate_user_key, key.user_key()) != 0) {
        return {};
    }

    if (DecodeInternalKeyValueKind(entry.internal_key) == ValueKind::Deletion) {
        return {
            .kind = MemTableLookupKind::Deletion,
            .value = {},
        };
    }
    return {
        .kind = MemTableLookupKind::Value,
        .value = entry.value,
    };
}

int MemTable::EntryComparator::operator()(const std::byte* left,
                                          const std::byte* right) const noexcept {
    return comparator.Compare(DecodeInternalKey(left), DecodeInternalKey(right));
}

MemTable::Iterator::Iterator(const MemTable& table) noexcept : iterator_(table.table_) {}

ByteView MemTable::Iterator::key() const { return DecodeEntry(iterator_.entry()).internal_key; }

ByteView MemTable::Iterator::value() const { return DecodeEntry(iterator_.entry()).value; }

Status MemTable::Iterator::Seek(ByteView internal_key) {
    const Result<InternalKeyView> parsed = ParseInternalKey(internal_key);
    if (!parsed.has_value()) {
        return std::unexpected(parsed.error());
    }

    SeekEncoded(internal_key);
    return {};
}

void MemTable::Iterator::SeekTrusted(ByteView internal_key) {
    assert(ParseInternalKey(internal_key).has_value());
    SeekEncoded(internal_key);
}

void MemTable::Iterator::SeekEncoded(ByteView internal_key) {
    const std::size_t prefix_size = VarintLength(static_cast<std::uint32_t>(internal_key.size()));
    seek_key_.resize(prefix_size + internal_key.size());
    MutableByteView output(seek_key_);
    EncodeLengthPrefixedTrusted(output, internal_key);
    assert(output.empty());
    iterator_.Seek(seek_key_.data());
}

}  // namespace modern_leveldb
