#include "engine/lookup.h"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

#if MODERN_LEVELDB_READ_DIAGNOSTICS
#include "instrumentation/read_diagnostics.h"
#endif
#include "metadata/version_edit.h"
#include "modern_leveldb/base/bytes.h"
#include "modern_leveldb/base/comparator.h"

namespace modern_leveldb {
namespace {

// The decision of one source: the value or the deletion of its newest visible
// entry, or nothing if it has none.
enum class Decision {
    Missing,
    Value,
    Deletion,
};

Result<Decision> SearchFile(TableCache& table_cache, const FileMetadata& file, const LookupKey& key,
                            std::vector<std::byte>& value, const TableReadOptions& options) {
#if MODERN_LEVELDB_READ_DIAGNOSTICS
    read_diagnostics::Add(read_diagnostics::Counter::FilesSearched);
#endif
    const Result<TableCache::Handle> table = table_cache.Find(file.number, file.file_size);
    if (!table.has_value()) {
        return std::unexpected(table.error());
    }
    const Result<TableLookupKind> found = (*table)->Get(key, value, options);
    if (!found.has_value()) {
        return std::unexpected(std::move(found).error());
    }
    switch (*found) {  // GCOVR_EXCL_BR_WITHOUT_HIT: 1/4 implicit default of an exhaustive enum
        case TableLookupKind::Missing:
            return Decision::Missing;
        case TableLookupKind::Value:
            return Decision::Value;
        case TableLookupKind::Deletion:
            return Decision::Deletion;
    }
    // GCOVR_EXCL_START: validated internal enum is exhaustive
    return std::unexpected(Error::Corruption("unknown table lookup result"));
    // GCOVR_EXCL_STOP
}

// A file of the version that a point read searches, at its level.
struct Candidate {
    std::uint32_t level;
    const Version::File* file;
};

SeekCharge ChargeOf(const Candidate& candidate) {
    SeekCharge charge{.level = candidate.level, .file = candidate.file};
    return charge;
}

class SelectionTimer final {
public:
    SelectionTimer() noexcept
#if MODERN_LEVELDB_READ_DIAGNOSTICS
        : timer_(read_diagnostics::Stage::CandidateSelection)
#endif
    {
    }

    void Resume() noexcept {
#if MODERN_LEVELDB_READ_DIAGNOSTICS
        timer_.Resume();
#endif
    }

    void Pause() noexcept {
#if MODERN_LEVELDB_READ_DIAGNOSTICS
        timer_.Pause();
#endif
    }

private:
#if MODERN_LEVELDB_READ_DIAGNOSTICS
    read_diagnostics::StageAccumulator timer_;
#endif
};

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

template <typename Visitor>
Status ForEachOverlapping(const Version& version, const InternalKeyComparator& comparator,
                          ByteView user_key, ByteView internal_key, SelectionTimer& timer,
                          Visitor&& visitor) {
    using VisitorResult = std::remove_cvref_t<std::invoke_result_t<Visitor&, const Candidate&>>;
    static_assert(std::is_same_v<VisitorResult, bool> ||
                  std::is_same_v<VisitorResult, Result<bool>>);
    timer.Resume();
    std::vector<const Version::File*> level0;
    level0.reserve(version.files(0).size());
    for (const Version::File& file : version.files(0)) {
        if (comparator.user_comparator().Compare(user_key, file->smallest.user_key()) >= 0 &&
            comparator.user_comparator().Compare(user_key, file->largest.user_key()) <= 0) {
            level0.push_back(&file);
        }
    }
    // Level-0 files are numbered in the order they were written.
    std::ranges::sort(level0, std::ranges::greater{},
                      [](const Version::File* file) { return (**file).number; });
#if MODERN_LEVELDB_READ_DIAGNOSTICS
    read_diagnostics::Add(read_diagnostics::Counter::Level0Candidates, level0.size());
#endif
    timer.Pause();

    for (const Version::File* file : level0) {
        const Candidate candidate{.level = 0, .file = file};
        if constexpr (std::is_same_v<VisitorResult, bool>) {
            if (!visitor(candidate)) {
                return {};
            }
        } else {
            Result<bool> keep_going = visitor(candidate);
            if (!keep_going.has_value()) {
                return std::unexpected(std::move(keep_going).error());
            }
            if (!*keep_going) {
                return {};
            }
        }
    }

    for (std::uint32_t level = 1; level < NumLevels; ++level) {
        timer.Resume();
        const Version::File* file =
            LevelCandidate(version.files(level), comparator, user_key, internal_key);
        if (file != nullptr) {
#if MODERN_LEVELDB_READ_DIAGNOSTICS
            read_diagnostics::Add(read_diagnostics::Counter::DeeperCandidates);
#endif
        }
        timer.Pause();
        if (file == nullptr) {
            continue;
        }
        if constexpr (std::is_same_v<VisitorResult, bool>) {
            if (!visitor(Candidate{.level = level, .file = file})) {
                return {};
            }
        } else {
            Result<bool> keep_going = visitor(Candidate{.level = level, .file = file});
            if (!keep_going.has_value()) {
                return std::unexpected(std::move(keep_going).error());
            }
            if (!*keep_going) {
                return {};
            }
        }
    }
    return {};
}

}  // namespace

Result<bool> LookupValue(const MemTable& memtable, const MemTable* immutable,
                         const Version& version, TableCache& table_cache,
                         const InternalKeyComparator& comparator, const LookupKey& key,
                         std::vector<std::byte>& value, std::optional<SeekCharge>& seek,
                         const TableReadOptions& options) {
    seek.reset();
    for (const MemTable* source : {&memtable, immutable}) {
        if (source == nullptr) {
            continue;
        }
        const MemTableLookup found = source->Lookup(key);
        if (found.kind == MemTableLookupKind::Value) {
#if MODERN_LEVELDB_READ_DIAGNOSTICS
            read_diagnostics::Add(source == &memtable ? read_diagnostics::Counter::MutableHits
                                                      : read_diagnostics::Counter::ImmutableHits);
            {
                read_diagnostics::StageScope copy(read_diagnostics::Stage::ResultCopy);
                read_diagnostics::Add(read_diagnostics::Counter::ResultBytes, found.value.size());
                value.resize(found.value.size());
                std::ranges::copy(found.value, value.begin());
            }
#else
            value.resize(found.value.size());
            std::ranges::copy(found.value, value.begin());
#endif
            return true;
        }
        if (found.kind == MemTableLookupKind::Deletion) {
#if MODERN_LEVELDB_READ_DIAGNOSTICS
            read_diagnostics::Add(source == &memtable ? read_diagnostics::Counter::MutableHits
                                                      : read_diagnostics::Counter::ImmutableHits);
            read_diagnostics::Add(read_diagnostics::Counter::Deletions);
#endif
            return false;
        }
    }

    SelectionTimer selection;
    std::optional<Candidate> previous;
    bool decided = false;
    bool found_value = false;
    const Status visited =
        ForEachOverlapping(version, comparator, key.user_key(), key.internal_key(), selection,
                           [&](const Candidate& candidate) -> Result<bool> {
                               if (!seek.has_value() && previous.has_value()) {
                                   seek = ChargeOf(*previous);
                               }
                               previous = candidate;

                               Result<Decision> decision =
                                   SearchFile(table_cache, **candidate.file, key, value, options);
                               if (!decision.has_value()) {
                                   return std::unexpected(std::move(decision).error());
                               }
                               if (*decision == Decision::Missing) {
                                   return true;
                               }
#if MODERN_LEVELDB_READ_DIAGNOSTICS
                               read_diagnostics::Add(read_diagnostics::Counter::SstableHits);
                               if (*decision == Decision::Deletion) {
                                   read_diagnostics::Add(read_diagnostics::Counter::Deletions);
                               }
#endif
                               found_value = *decision == Decision::Value;
                               decided = true;
                               return false;
                           });
    if (!visited.has_value()) {
        return std::unexpected(visited.error());
    }
    if (decided) {
        return found_value;
    }
#if MODERN_LEVELDB_READ_DIAGNOSTICS
    read_diagnostics::Add(read_diagnostics::Counter::Misses);
#endif
    return false;
}

std::optional<SeekCharge> SampleCharge(const Version& version,
                                       const InternalKeyComparator& comparator,
                                       ByteView internal_key) {
    const Result<ParsedInternalKey> parsed = ParseInternalKey(internal_key);
    assert(parsed.has_value());
    SelectionTimer selection;
    std::optional<Candidate> first;
    std::size_t matches = 0;
    const Status visited =
        ForEachOverlapping(version, comparator, parsed->user_key, internal_key, selection,
                           [&](const Candidate& candidate) {  // GCOVR_EXCL_LINE: GCC lambda clone
                               ++matches;
                               if (!first.has_value()) {
                                   first = candidate;
                               }
                               return matches < 2;
                           });
    assert(visited.has_value());
    (void)visited;
    if (matches < 2) {
        return std::nullopt;
    }
    return ChargeOf(*first);
}

}  // namespace modern_leveldb
