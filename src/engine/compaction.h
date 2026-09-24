#ifndef MODERN_LEVELDB_ENGINE_COMPACTION_H_
#define MODERN_LEVELDB_ENGINE_COMPACTION_H_

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>

#include "engine/compaction_picker.h"
#include "engine/internal_iterator.h"
#include "engine/table_cache.h"
#include "format/internal_key.h"
#include "metadata/version_edit.h"
#include "modern_leveldb/base/result.h"
#include "platform/file_system.h"
#include "table/table_builder.h"

namespace modern_leveldb {

struct CompactionOptions {
  TableBuilderOptions table_options{};
  // LevelDB's max_file_size: an output ends once it reaches this size, or
  // early once the grandparents it overlaps total more than ten times this.
  std::uint64_t target_file_size = std::uint64_t{2} << 20U;
};

// The engine's steps within a compaction, which may take the database mutex.
struct CompactionHooks {
  // Returns a fresh file number for an output and protects it from
  // obsolete-file cleanup.
  std::function<std::uint64_t()> new_file_number;
  // Runs before each input entry, where the engine flushes a pending immutable
  // memtable. An error stops the compaction, such as when the database closes.
  std::function<Status()> before_entry;
};

// Merges the compaction's inputs as LevelDB's VersionSet::MakeInputIterator
// does: each level-0 input on its own, and the inputs of a deeper level as one
// run of files, reading without filling the block cache. Creating it performs
// no I/O. It holds the compaction's version and borrows the compaction's input
// lists, so the compaction must outlive it without changing them; the table
// cache and the comparator must outlive it too.
[[nodiscard]] std::unique_ptr<InternalIterator> NewCompactionIterator(
    const Compaction& compaction, TableCache& table_cache, const InternalKeyComparator& comparator);
std::unique_ptr<InternalIterator> NewCompactionIterator(
    const Compaction&& compaction, TableCache& table_cache,
    const InternalKeyComparator& comparator) = delete;
std::unique_ptr<InternalIterator> NewCompactionIterator(
    const Compaction& compaction, TableCache& table_cache,
    const InternalKeyComparator&& comparator) = delete;

// Runs the compaction over the input as LevelDB's DBImpl::DoCompactionWork
// does, and returns CompactionEdit(compaction) with every output added to
// level `level + 1` in key order. An entry is dropped if an earlier entry of
// its user key has a sequence at most the smallest snapshot, and a deletion
// whose sequence is at most the smallest snapshot is dropped if no file in the
// levels below the next one holds its user key. The other entries go to
// outputs that end once they reach the target file size, or before an entry
// once the grandparents that the entries passed total more than ten times it.
// Each output is finished durably and opened through the table cache, and then
// the directory is synced once.
//
// Calls before_entry before each entry and new_file_number for each output.
// Returns the first error of the hooks, the input, the table builder, or a file
// operation, and Corruption for a key that is not an internal key; outputs
// written before an error stay for obsolete-file cleanup. Touches only its
// arguments, so the caller need not hold the database mutex.
[[nodiscard]] Result<VersionEdit> RunCompaction(
    FileSystem& file_system, const std::filesystem::path& directory,
    const InternalKeyComparator& comparator, const CompactionOptions& options,
    TableCache& table_cache, const Compaction& compaction, InternalIterator& input,
    SequenceNumber smallest_snapshot, const CompactionHooks& hooks);

}  // namespace modern_leveldb

#endif  // MODERN_LEVELDB_ENGINE_COMPACTION_H_
