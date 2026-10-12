#include "engine/lookup.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <functional>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>
#include <vector>

#include "base/bytes_internal.h"
#include "base/result_internal.h"
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

Decision SearchMemTable(const MemTable& source, const LookupKey& key,
                        std::vector<std::byte>& value) {
    const MemTableLookup found = source.Lookup(key);
    if (found.kind == MemTableLookupKind::Value) {
        CopyLookupValue(found.value, value);
        return Decision::Value;
    }
    if (found.kind == MemTableLookupKind::Deletion) {
        return Decision::Deletion;
    }
    return Decision::Missing;
}

std::optional<bool> SearchMemory(const MemTable& memtable, const MemTable* immutable,
                                 const LookupKey& key, std::vector<std::byte>& value) {
    for (const MemTable* source : {&memtable, immutable}) {
        if (source == nullptr) {
            continue;
        }
        const Decision decision = SearchMemTable(*source, key, value);
        if (decision != Decision::Missing) {
            return decision == Decision::Value;
        }
    }
    return std::nullopt;
}

Result<Decision> SearchFile(TableCache& table_cache, const FileMetadata& file, const LookupKey& key,
                            std::vector<std::byte>& value, const TableReadOptions& options) {
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

struct PointLookup {
    TableCache& table_cache;
    const LookupKey& key;
    std::vector<std::byte>& value;
    std::optional<SeekCharge>& seek;
    const TableReadOptions& options;
    std::optional<Candidate> previous;
    bool decided = false;
    bool found_value = false;

    Result<bool> Visit(const Candidate& candidate) {
        if (!seek.has_value() && previous.has_value()) {
            seek = ChargeOf(*previous);
        }
        previous = candidate;

        Result<Decision> decision = SearchFile(table_cache, **candidate.file, key, value, options);
        if (!decision.has_value()) {
            return std::unexpected(std::move(decision).error());
        }
        if (*decision == Decision::Missing) {
            return true;
        }
        found_value = *decision == Decision::Value;
        decided = true;
        return false;
    }

    [[nodiscard]] bool result() const noexcept { return decided && found_value; }
};

struct SampledOverlap {
    std::optional<Candidate> first;
    std::size_t matches = 0;

    bool Visit(const Candidate& candidate) noexcept {
        ++matches;
        if (!first.has_value()) {
            first = candidate;
        }
        return matches < 2;
    }

    [[nodiscard]] std::optional<SeekCharge> charge() const noexcept {
        if (matches < 2) {
            return std::nullopt;
        }
        return ChargeOf(*first);
    }
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

bool UserKeyInFileRange(const FileMetadata& file, const Comparator& comparator, ByteView user_key) {
    return comparator.Compare(user_key, file.smallest.user_key()) >= 0 &&
           comparator.Compare(user_key, file.largest.user_key()) <= 0;
}

std::vector<const Version::File*> LevelZeroCandidates(std::span<const Version::File> files,
                                                      const Comparator& comparator,
                                                      ByteView user_key) {
    std::vector<const Version::File*> level0;
    level0.reserve(files.size());
    for (const Version::File& file : files) {
        if (UserKeyInFileRange(*file, comparator, user_key)) {
            level0.push_back(&file);
        }
    }
    // Overlapping level-0 files must be searched newest first: an older value
    // must not win over a later overwrite or deletion.
    std::ranges::sort(level0, std::ranges::greater{},
                      [](const Version::File* file) { return (**file).number; });
    return level0;
}

// L0 ordering is prepared eagerly; deeper levels are selected only on advancement.
struct CandidateCursor {
    const Version& version;
    const InternalKeyComparator& comparator;
    ByteView user_key;
    ByteView internal_key;
    std::vector<const Version::File*> level0;
    std::size_t level0_position = 0;
    std::uint32_t next_level = 1;

    std::optional<Candidate> NextLevelZero() {
        if (level0_position == level0.size()) {
            return std::nullopt;
        }
        return Candidate{.level = 0, .file = level0[level0_position++]};
    }

    std::optional<Candidate> NextDeeperLevel() {
        while (next_level < NumLevels) {
            const std::uint32_t level = next_level++;
            const Version::File* file =
                LevelCandidate(version.files(level), comparator, user_key, internal_key);
            if (file != nullptr) {
                return Candidate{.level = level, .file = file};
            }
        }
        return std::nullopt;
    }

    std::optional<Candidate> Next() {
        if (auto candidate = NextLevelZero(); candidate.has_value()) {
            return candidate;
        }
        return NextDeeperLevel();
    }
};

template <typename Visitor>
Status ForEachOverlapping(const Version& version, const InternalKeyComparator& comparator,
                          ByteView user_key, ByteView internal_key, Visitor&& visitor) {
    using VisitorResult = std::remove_cvref_t<std::invoke_result_t<Visitor&, const Candidate&>>;
    static_assert(std::is_same_v<VisitorResult, bool> ||
                  std::is_same_v<VisitorResult, Result<bool>>);
    CandidateCursor candidates{
        .version = version,
        .comparator = comparator,
        .user_key = user_key,
        .internal_key = internal_key,
        .level0 = LevelZeroCandidates(version.files(0), comparator.user_comparator(), user_key)};
    while (const auto candidate = candidates.Next()) {
        if constexpr (std::is_same_v<VisitorResult, bool>) {
            if (!visitor(*candidate)) {
                return {};
            }
        } else {
            Result<bool> keep_going = visitor(*candidate);
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
    // Newest source with a visible entry decides. A tombstone decides absence,
    // unlike Missing, which permits searching older sources.
    if (const auto decision = SearchMemory(memtable, immutable, key, value); decision.has_value()) {
        return *decision;
    }

    PointLookup lookup{
        .table_cache = table_cache, .key = key, .value = value, .seek = seek, .options = options};
    const Status visited = ForEachOverlapping(
        version, comparator, key.user_key(), key.internal_key(),
        [&](const Candidate& candidate) -> Result<bool> { return lookup.Visit(candidate); });
    if (!visited.has_value()) {
        return std::unexpected(visited.error());
    }
    return lookup.result();
}

std::optional<SeekCharge> SampleCharge(const Version& version,
                                       const InternalKeyComparator& comparator,
                                       ByteView internal_key) {
    const Result<InternalKeyView> parsed = ParseInternalKey(internal_key);
    SampledOverlap sampled;
    static_cast<void>(
        ForEachOverlapping(version, comparator, parsed->user_key, internal_key,
                           [&](const Candidate& candidate) {  // GCOVR_EXCL_LINE: GCC lambda clone
                               return sampled.Visit(candidate);
                           }));
    return sampled.charge();
}

}  // namespace modern_leveldb
