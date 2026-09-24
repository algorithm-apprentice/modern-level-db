#ifndef MODERN_LEVELDB_ENGINE_ITERATORS_H_
#define MODERN_LEVELDB_ENGINE_ITERATORS_H_

#include <memory>
#include <span>
#include <vector>

#include "engine/internal_iterator.h"
#include "engine/table_cache.h"
#include "format/internal_key.h"
#include "memory/memtable.h"
#include "metadata/version.h"
#include "table/table.h"

namespace modern_leveldb {

// Iterates the memtable, which the iterator holds. A seek fails only for a
// target over 4 GiB, which no internal key of a memtable or a table reaches,
// and then leaves the iterator invalid.
[[nodiscard]] std::unique_ptr<InternalIterator> NewMemTableIterator(
    std::shared_ptr<const MemTable> memtable);

// Iterates files that are sorted by key and do not overlap, such as a level of
// the version or a run of its files, opening each table through the table
// cache when the iterator reaches it. The iterator holds the version, which
// must hold the files, so that they stay live. A table without entries is
// Corruption.
[[nodiscard]] std::unique_ptr<InternalIterator> NewLevelIterator(
    std::shared_ptr<const Version> version, std::span<const Version::File> files,
    TableCache& table_cache, const InternalKeyComparator& comparator,
    const TableReadOptions& options);
std::unique_ptr<InternalIterator> NewLevelIterator(std::shared_ptr<const Version> version,
                                                   std::span<const Version::File> files,
                                                   TableCache& table_cache,
                                                   const InternalKeyComparator&& comparator,
                                                   const TableReadOptions& options) = delete;

// Merges the children. Among equal keys, it yields the first child's entry
// first when it moves forward and the last child's when it moves backward.
// When it changes direction, it positions every other child strictly after or
// before the current key.
[[nodiscard]] std::unique_ptr<InternalIterator> NewMergingIterator(
    const InternalKeyComparator& comparator,
    std::vector<std::unique_ptr<InternalIterator>> children);
std::unique_ptr<InternalIterator> NewMergingIterator(
    const InternalKeyComparator&& comparator,
    std::vector<std::unique_ptr<InternalIterator>> children) = delete;

// Merges the memtable, the immutable memtable if there is one, each level-0
// file of the version, and each deeper level with files. Creating it performs
// no I/O. The table cache and the comparator must outlive it.
[[nodiscard]] std::unique_ptr<InternalIterator> NewInternalIterator(
    std::shared_ptr<const MemTable> memtable, std::shared_ptr<const MemTable> immutable,
    std::shared_ptr<const Version> version, TableCache& table_cache,
    const InternalKeyComparator& comparator, const TableReadOptions& options);
std::unique_ptr<InternalIterator> NewInternalIterator(std::shared_ptr<const MemTable> memtable,
                                                      std::shared_ptr<const MemTable> immutable,
                                                      std::shared_ptr<const Version> version,
                                                      TableCache& table_cache,
                                                      const InternalKeyComparator&& comparator,
                                                      const TableReadOptions& options) = delete;

}  // namespace modern_leveldb

#endif  // MODERN_LEVELDB_ENGINE_ITERATORS_H_
