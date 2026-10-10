#ifndef MODERN_LEVELDB_ENGINE_SEEK_STATISTICS_H_
#define MODERN_LEVELDB_ENGINE_SEEK_STATISTICS_H_

#include <cstdint>
#include <functional>
#include <optional>

#include "engine/compaction_picker.h"
#include "engine/lookup.h"
#include "metadata/version.h"

namespace modern_leveldb {

// The average number of bytes that an iterator reads between samples, LevelDB's
// config::kReadBytesPeriod.
inline constexpr std::uint64_t ReadBytesPeriod = std::uint64_t{1} << 20U;

// Returns an iterator's sampling periods as LevelDB draws them for the seed:
// uniform below twice ReadBytesPeriod, from LevelDB's Random.
[[nodiscard]] std::function<std::uint64_t()> ReadSamplingPeriods(std::uint32_t seed);

// The file that the current version should compact because its metadata seek
// budget ran out. It is not thread-safe; the engine uses it with the database
// mutex held.
class SeekStatistics final {
public:
    // Charges a seek to the exact file slot in `version` at the charge's level.
    // Its metadata budget keeps falling below zero. If the budget is then at
    // most zero, `version` is `current`, and no file is recorded for `current`,
    // borrows that slot and returns true.
    [[nodiscard]] bool Charge(const Version& version, const Version& current,
                              const SeekCharge& charge);

    // Reports a recorded file without materializing its shared owner.
    [[nodiscard]] bool HasFileToCompact(const Version& current) const noexcept;

    // Returns an owning file for background compaction.
    [[nodiscard]] std::optional<SeekCompaction> FileToCompact(const Version& current) const;

    // Forgets a file recorded for another version.
    void Retain(const Version& current);

private:
    struct Recorded {
        const Version* version;
        std::uint32_t level;
        const Version::File* file;
    };

    [[nodiscard]] bool IsRecordedFor(const Version& version) const noexcept;

    std::optional<Recorded> recorded_;
};

}  // namespace modern_leveldb

#endif  // MODERN_LEVELDB_ENGINE_SEEK_STATISTICS_H_
