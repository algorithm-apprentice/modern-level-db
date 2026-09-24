#include "engine/lookup.h"

#include <algorithm>
#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <span>
#include <utility>
#include <vector>

#include "metadata/version_edit.h"
#include "modern_leveldb/base/bytes.h"
#include "modern_leveldb/base/comparator.h"

namespace modern_leveldb {
namespace {

using Value = std::optional<std::vector<std::byte>>;

// The decision of one source: the value or the deletion of its newest visible
// entry, or nothing if it has none.
using Decision = std::optional<Value>;

Result<Decision> SearchFile(TableCache& table_cache, const FileMetadata& file, const LookupKey& key,
                            const TableReadOptions& options) {
  const Result<TableCache::Handle> table = table_cache.Find(file.number, file.file_size);
  if (!table.has_value()) {
    return std::unexpected(table.error());
  }
  Result<std::optional<TableLookup>> found = (*table)->Get(key, options);
  if (!found.has_value()) {
    return std::unexpected(std::move(found).error());
  }
  if (!found->has_value()) {
    return Decision();
  }
  if ((*found)->kind == ValueKind::Deletion) {
    return Decision(Value());
  }
  return Decision(Value(std::move((*found)->value)));
}

// Returns the level-0 files whose user-key range holds the user key, newest
// first.
std::vector<const FileMetadata*> Level0Candidates(const Version& version,
                                                  const Comparator& user_comparator,
                                                  ByteView user_key) {
  std::vector<const FileMetadata*> candidates;
  for (const Version::File& file : version.files(0)) {
    if (user_comparator.Compare(user_key, file->smallest.user_key()) >= 0 &&
        user_comparator.Compare(user_key, file->largest.user_key()) <= 0) {
      candidates.push_back(file.get());
    }
  }
  // Level-0 files are numbered in the order they were written.
  std::ranges::sort(candidates, std::ranges::greater{}, &FileMetadata::number);
  return candidates;
}

// Returns the only file of a deeper level that may hold the key: the first one
// whose largest key is not before it, if its smallest user key is not after the
// user key.
const FileMetadata* LevelCandidate(std::span<const Version::File> files,
                                   const InternalKeyComparator& comparator, const LookupKey& key) {
  const auto found = std::ranges::partition_point(files, [&](const Version::File& file) {
    return comparator.Compare(file->largest.encoded(), key.internal_key()) < 0;
  });
  if (found == files.end() ||
      comparator.user_comparator().Compare(key.user_key(), (*found)->smallest.user_key()) < 0) {
    return nullptr;
  }
  return found->get();
}

// Returns the level-0 candidates followed by each deeper level's candidate.
std::vector<const FileMetadata*> Candidates(const Version& version,
                                            const InternalKeyComparator& comparator,
                                            const LookupKey& key) {
  std::vector<const FileMetadata*> candidates =
      Level0Candidates(version, comparator.user_comparator(), key.user_key());
  for (std::uint32_t level = 1; level < NumLevels; ++level) {
    const FileMetadata* file = LevelCandidate(version.files(level), comparator, key);
    if (file != nullptr) {
      candidates.push_back(file);
    }
  }
  return candidates;
}

}  // namespace

Result<std::optional<std::vector<std::byte>>> LookupValue(
    const MemTable& memtable, const MemTable* immutable, const Version& version,
    TableCache& table_cache, const InternalKeyComparator& comparator, const LookupKey& key,
    const TableReadOptions& options) {
  for (const MemTable* source : {&memtable, immutable}) {
    if (source == nullptr) {
      continue;
    }
    const MemTableLookup found = source->Lookup(key);
    if (found.kind == MemTableLookupKind::Value) {
      return Value(std::vector<std::byte>(found.value.begin(), found.value.end()));
    }
    if (found.kind == MemTableLookupKind::Deletion) {
      return Value();
    }
  }

  for (const FileMetadata* file : Candidates(version, comparator, key)) {
    Result<Decision> decision = SearchFile(table_cache, *file, key, options);
    if (!decision.has_value()) {
      return std::unexpected(std::move(decision).error());
    }
    if (decision->has_value()) {
      return std::move(**decision);
    }
  }
  return Value();
}

}  // namespace modern_leveldb
