#include "memory/memtable.h"

#include <cstddef>
#include <cstdint>

#include "base/coding_internal.h"
#include "format/internal_key.h"
#include "modern_leveldb/base/bytes.h"
#include "modern_leveldb/base/coding.h"
#include "modern_leveldb/base/comparator.h"
#include "modern_leveldb/base/result.h"

namespace modern_leveldb {
namespace {

// Private packed-record codec for MemTable storage. Shared length-prefix and
// internal-key primitives stay in base/format; this representation has no
// consumer outside MemTable and its SkipList comparator/iterator.
struct MemTableEntryView {
    ByteView internal_key;
    ByteView value;
};

bool MatchesMemTableUserKey(const Comparator& comparator, ByteView internal_key,
                            ByteView user_key) noexcept {
    const ByteView candidate_user_key =
        internal_key.first(internal_key.size() - InternalKeyTrailerSize);
    return comparator.Compare(candidate_user_key, user_key) == 0;
}

MemTableLookup ClassifyMemTableEntry(const MemTableEntryView& entry) noexcept {
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

ByteView DecodeMemTableInternalKey(const std::byte* entry) noexcept {
    return ConsumeLengthPrefixedTrusted(entry);
}

MemTableEntryView DecodeMemTableEntry(const std::byte* entry) noexcept {
    const ByteView internal_key = ConsumeLengthPrefixedTrusted(entry);
    const ByteView value = ConsumeLengthPrefixedTrusted(entry);
    return {
        .internal_key = internal_key,
        .value = value,
    };
}

std::size_t EncodedMemTableEntrySize(ByteView key, ByteView value) noexcept {
    const std::size_t internal_key_size = key.size() + InternalKeyTrailerSize;
    return VarintLength(static_cast<std::uint32_t>(internal_key_size)) + internal_key_size +
           VarintLength(static_cast<std::uint32_t>(value.size())) + value.size();
}

const std::byte* EncodeMemTableEntry(Arena& arena, SequenceNumber sequence, ValueKind kind,
                                     ByteView key, ByteView value, std::size_t encoded_size) {
    MutableByteView output(arena.Allocate(encoded_size), encoded_size);
    const std::byte* const entry = output.data();
    const std::size_t internal_key_size = key.size() + InternalKeyTrailerSize;
    EncodeVarint32Trusted(output, static_cast<std::uint32_t>(internal_key_size));

    EncodeInternalKeyTrusted(output, key, sequence, kind);

    EncodeLengthPrefixedTrusted(output, value);
    return entry;
}

}  // namespace

MemTable::MemTable(const Comparator& user_comparator)
    : entry_comparator_(user_comparator), table_(entry_comparator_, arena_) {}

void MemTable::Add(SequenceNumber sequence, ValueKind kind, ByteView key, ByteView value) {
    const std::byte* const entry = EncodeMemTableEntry(arena_, sequence, kind, key, value,
                                                       EncodedMemTableEntrySize(key, value));
    table_.Insert(entry);
}

MemTableLookup MemTable::Lookup(const LookupKey& key) const {
    // Descending sequence order skips versions newer than the lookup's sequence.
    // A lower-bound result still needs equality on the user-key portion.
    Table::Iterator iterator(table_);
    iterator.Seek(key.internal_key());
    if (!iterator.valid()) {
        return {};
    }

    const MemTableEntryView entry = DecodeMemTableEntry(iterator.entry());
    if (!MatchesMemTableUserKey(entry_comparator_.user_comparator(), entry.internal_key,
                                key.user_key())) {
        return {};
    }
    return ClassifyMemTableEntry(entry);
}

int MemTable::EntryComparator::operator()(const std::byte* left,
                                          const std::byte* right) const noexcept {
    return (*this)(left, DecodeMemTableInternalKey(right));
}

int MemTable::EntryComparator::operator()(const std::byte* left, ByteView right) const noexcept {
    return internal_comparator.Compare(DecodeMemTableInternalKey(left), right);
}

MemTable::Iterator::Iterator(const MemTable& table) noexcept : iterator_(table.table_) {}

ByteView MemTable::Iterator::key() const {
    return DecodeMemTableEntry(iterator_.entry()).internal_key;
}

ByteView MemTable::Iterator::value() const { return DecodeMemTableEntry(iterator_.entry()).value; }

void MemTable::Iterator::Seek(ByteView internal_key) { iterator_.Seek(internal_key); }

}  // namespace modern_leveldb
