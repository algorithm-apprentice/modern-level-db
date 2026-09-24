#include "engine/lookup.h"

#include <algorithm>
#include <cassert>
#include <cstddef>
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

// A file of the version that a point read searches, at its level.
struct Candidate {
  std::uint32_t level;
  const Version::File* file;
};

SeekCharge ChargeOf(const Candidate& candidate) {
  SeekCharge charge{.level = candidate.level, .file = *candidate.file};
  return charge;
}

// Returns the level-0 files whose user-key range holds the user key, newest
// first.
std::vector<Candidate> Level0Candidates(const Version& version, const Comparator& user_comparator,
                                        ByteView user_key) {
  std::vector<Candidate> candidates;
  for (const Version::File& file : version.files(0)) {
    if (user_comparator.Compare(user_key, file->smallest.user_key()) >= 0 &&
        user_comparator.Compare(user_key, file->largest.user_key()) <= 0) {
      candidates.push_back(Candidate{.level = 0, .file = &file});
    }
  }
  // Level-0 files are numbered in the order they were written.
  std::ranges::sort(candidates, std::ranges::greater{},
                    [](const Candidate& candidate) { return (*candidate.file)->number; });
  return candidates;
}

// Returns the only file of a deeper level that may hold the key: the first one
// whose largest key is not before it, if its smallest user key is not after the
// user key.
const Version::File* LevelCandidate(std::span<const Version::File> files,
                                    const InternalKeyComparator& comparator, ByteView user_key,
                                    ByteView internal_key) {
  const auto found = std::ranges::partition_point(files, [&](const Version::File& file) {
    return comparator.Compare(file->largest.encoded(), internal_key) < 0;
  });
  if (found == files.end() ||
      comparator.user_comparator().Compare(user_key, (*found)->smallest.user_key()) < 0) {
    return nullptr;
  }
  return &*found;
}

// Returns the level-0 candidates followed by each deeper level's candidate.
std::vector<Candidate> Candidates(const Version& version, const InternalKeyComparator& comparator,
                                  ByteView user_key, ByteView internal_key) {
  std::vector<Candidate> candidates =
      Level0Candidates(version, comparator.user_comparator(), user_key);
  for (std::uint32_t level = 1; level < NumLevels; ++level) {
    const Version::File* file =
        LevelCandidate(version.files(level), comparator, user_key, internal_key);
    if (file != nullptr) {
      candidates.push_back(Candidate{.level = level, .file = file});
    }
  }
  return candidates;
}

}  // namespace

Result<PointRead> LookupValue(const MemTable& memtable, const MemTable* immutable,
                              const Version& version, TableCache& table_cache,
                              const InternalKeyComparator& comparator, const LookupKey& key,
                              const TableReadOptions& options) {
  PointRead read;
  for (const MemTable* source : {&memtable, immutable}) {
    if (source == nullptr) {
      continue;
    }
    const MemTableLookup found = source->Lookup(key);
    if (found.kind == MemTableLookupKind::Value) {
      read.value.emplace(found.value.begin(), found.value.end());
      return read;
    }
    if (found.kind == MemTableLookupKind::Deletion) {
      return read;
    }
  }

  const std::vector<Candidate> candidates =
      Candidates(version, comparator, key.user_key(), key.internal_key());
  for (std::size_t index = 0; index < candidates.size(); ++index) {
    Result<Decision> decision = SearchFile(table_cache, **candidates[index].file, key, options);
    if (!decision.has_value()) {
      return std::unexpected(std::move(decision).error());
    }
    if (decision->has_value()) {
      read.value = std::move(**decision);
      if (index > 0) {
        read.seek = ChargeOf(candidates.front());
      }
      return read;
    }
  }
  if (candidates.size() > 1) {
    read.seek = ChargeOf(candidates.front());
  }
  return read;
}

std::optional<SeekCharge> SampleCharge(const Version& version,
                                       const InternalKeyComparator& comparator,
                                       ByteView internal_key) {
  const Result<ParsedInternalKey> parsed = ParseInternalKey(internal_key);
  assert(parsed.has_value());
  const std::vector<Candidate> candidates =
      Candidates(version, comparator, parsed->user_key, internal_key);
  if (candidates.size() < 2) {
    return std::nullopt;
  }
  return ChargeOf(candidates.front());
}

}  // namespace modern_leveldb
