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
    // Ends an output at this size, or earlier when grandparent overlap exceeds
    // ten times it, bounding the work of a future compaction.
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

// Merges overlapping level-0 files individually and each deeper level as a
// disjoint run. Reads do not fill the block cache; construction performs no I/O.
// Retains the Version but borrows input lists: the unchanged compaction,
// table cache, and comparator must outlive the iterator.
[[nodiscard]] std::unique_ptr<InternalIterator> NewCompactionIterator(
    const Compaction& compaction, TableCache& table_cache, const InternalKeyComparator& comparator);
std::unique_ptr<InternalIterator> NewCompactionIterator(
    const Compaction&& compaction, TableCache& table_cache,
    const InternalKeyComparator& comparator) = delete;
std::unique_ptr<InternalIterator> NewCompactionIterator(
    const Compaction& compaction, TableCache& table_cache,
    const InternalKeyComparator&& comparator) = delete;

// Rewrites sorted history into level + 1 and returns the edit, not its installation.
// Drops a shadowed entry of either kind once a newer entry is visible to every
// active snapshot. An otherwise unshadowed tombstone must itself be visible to
// every snapshot and have no deeper value to hide. Other entries remain in order.
//
// Outputs split at target size or excessive passed-grandparent overlap. Each
// output is synced and checked through the table cache, then the directory's
// backend namespace barrier is requested once. MANIFEST publication is the caller's job.
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
