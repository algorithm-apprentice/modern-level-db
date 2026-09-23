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

enum class MemTableLookupKind {
  Missing,
  Value,
  Deletion,
};

struct MemTableLookup {
  MemTableLookupKind kind = MemTableLookupKind::Missing;
  ByteView value;
};

class MemTable final {
 private:
  struct EntryComparator {
    const InternalKeyComparator& comparator;

    [[nodiscard]] int operator()(const std::byte* left,
                                 const std::byte* right) const noexcept;
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

  [[nodiscard]] Status Add(SequenceNumber sequence, ValueKind kind, ByteView key,
                           ByteView value);
  [[nodiscard]] MemTableLookup Lookup(const LookupKey& key) const;
  [[nodiscard]] std::size_t memory_usage() const noexcept {
    return arena_.memory_usage();
  }

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
    void SeekToFirst() { iterator_.SeekToFirst(); }
    void SeekToLast() { iterator_.SeekToLast(); }

   private:
    Table::Iterator iterator_;
    std::vector<std::byte> seek_key_;
  };

 private:
  const Comparator& user_comparator_;
  InternalKeyComparator internal_comparator_;
  EntryComparator entry_comparator_;
  Arena arena_;
  Table table_;
};

}  // namespace modern_leveldb

#endif  // MODERN_LEVELDB_MEMORY_MEMTABLE_H_
