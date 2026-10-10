#ifndef MODERN_LEVELDB_MEMORY_MEMTABLE_H_
#define MODERN_LEVELDB_MEMORY_MEMTABLE_H_

#include <cstddef>
#include <cstdint>
#include <vector>

#include "format/internal_key.h"
#include "memory/arena.h"
#include "memory/skiplist.h"
#include "modern_leveldb/base/bytes.h"
#include "modern_leveldb/base/comparator.h"
#include "modern_leveldb/base/result.h"

namespace modern_leveldb {

class DatabaseEngine;

enum class MemTableLookupKind {
    Missing,
    Value,
    Deletion,
};

// Missing permits looking in older sources; Deletion decides absence and prevents
// an older value from resurfacing. A Value view borrows the memtable's arena.
struct MemTableLookup {
    MemTableLookupKind kind = MemTableLookupKind::Missing;
    ByteView value;
};

// Stores immutable versioned entries in an arena-backed skip list. One serialized
// writer can publish entries while readers traverse; sequence filtering controls
// visibility. The user comparator and this table must outlive readers/iterators.
// See docs/learning/03-memory-and-mvcc.md.
class MemTable final {
private:
    struct EntryComparator {
        const InternalKeyComparator& comparator;

        [[nodiscard]] int operator()(const std::byte* left, const std::byte* right) const noexcept;
    };

    using Table = SkipList<const std::byte*, EntryComparator>;

public:
    explicit MemTable(const Comparator& user_comparator);
    MemTable(Comparator&&) = delete;
    MemTable(const Comparator&&) = delete;

    MemTable(const MemTable&) = delete;
    MemTable& operator=(const MemTable&) = delete;
    MemTable(MemTable&&) = delete;
    MemTable& operator=(MemTable&&) = delete;
    ~MemTable() = default;

    // Caller-provided key/value lengths follow the persistent uint32 representation.
    [[nodiscard]] Status Add(SequenceNumber sequence, ValueKind kind, ByteView key, ByteView value);
    // Requires an owned-batch entry with representable lengths, a reserved sequence,
    // and a unique internal key, as supplied by InsertBatchTrusted.
    void AddTrusted(SequenceNumber sequence, ValueKind kind, ByteView key, ByteView value);
    [[nodiscard]] MemTableLookup Lookup(const LookupKey& key) const;
    [[nodiscard]] std::size_t memory_usage() const noexcept { return arena_.memory_usage(); }

    // Borrowing counter, not a shared owner. The engine retains a rotated table
    // while these pins exist; other callers must keep the table alive themselves.
    class ReadPin final {
    public:
        ReadPin(const ReadPin&) = delete;
        ReadPin& operator=(const ReadPin&) = delete;
        ReadPin(ReadPin&& source) noexcept;
        ReadPin& operator=(ReadPin&&) = delete;
        ~ReadPin();

        [[nodiscard]] const MemTable& value() const noexcept;

    private:
        friend class MemTable;

        explicit ReadPin(const MemTable& table) noexcept;

        const MemTable* table_ = nullptr;
    };

    // The caller externally synchronizes pin creation and destruction.
    [[nodiscard]] ReadPin PinRead() const noexcept { return ReadPin(*this); }

    class Iterator final {
    public:
        explicit Iterator(const MemTable& table) noexcept;

        Iterator(const Iterator&) = delete;
        Iterator& operator=(const Iterator&) = delete;
        Iterator(Iterator&&) = delete;
        Iterator& operator=(Iterator&&) = delete;
        ~Iterator() = default;

        [[nodiscard]] bool valid() const noexcept { return iterator_.valid(); }
        [[nodiscard]] ByteView key() const;
        [[nodiscard]] ByteView value() const;
        void Next() { iterator_.Next(); }
        void Prev() { iterator_.Prev(); }
        [[nodiscard]] Status Seek(ByteView internal_key);
        // Requires a valid internally constructed internal key.
        void SeekTrusted(ByteView internal_key);
        void SeekToFirst() { iterator_.SeekToFirst(); }
        void SeekToLast() { iterator_.SeekToLast(); }

    private:
        void SeekEncoded(ByteView internal_key);

        Table::Iterator iterator_;
        std::vector<std::byte> seek_key_;
    };

private:
    friend class DatabaseEngine;

    const Comparator& user_comparator_;
    InternalKeyComparator internal_comparator_;
    EntryComparator entry_comparator_;
    Arena arena_;
    Table table_;
    mutable std::size_t read_pins_ = 0;
};

}  // namespace modern_leveldb

#endif  // MODERN_LEVELDB_MEMORY_MEMTABLE_H_
