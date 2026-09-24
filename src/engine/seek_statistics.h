#ifndef MODERN_LEVELDB_ENGINE_SEEK_STATISTICS_H_
#define MODERN_LEVELDB_ENGINE_SEEK_STATISTICS_H_

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
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

// The seek budgets of the files that reads charge, and the file that the
// current version should compact because its budget ran out. It is not
// thread-safe; the engine uses it with the database mutex held.
class SeekStatistics final {
 public:
  // Charges a seek to the file, which `version` holds at the charge's level.
  // The file's budget starts at its size divided by 16 KiB, and at least 100,
  // and keeps falling below zero. If the budget is then at most zero, `version`
  // is `current`, and no file is recorded for `current`, records the file for
  // `current` and returns true.
  [[nodiscard]] bool Charge(const std::shared_ptr<const Version>& version,
                            const std::shared_ptr<const Version>& current,
                            const SeekCharge& charge);

  // Returns the file recorded for `current`.
  [[nodiscard]] std::optional<SeekCompaction> FileToCompact(
      const std::shared_ptr<const Version>& current) const;

  // Forgets the budgets of the files that `current` does not hold, and a file
  // recorded for another version.
  void Retain(const Version& current);

 private:
  [[nodiscard]] bool IsRecordedFor(const std::shared_ptr<const Version>& version) const;

  // Keyed by each file's metadata, which a budget keeps alive, so that no other
  // metadata can take its place.
  std::map<Version::File, std::int64_t> budgets_;
  std::weak_ptr<const Version> recorded_version_;
  std::optional<SeekCompaction> recorded_;
};

}  // namespace modern_leveldb

#endif  // MODERN_LEVELDB_ENGINE_SEEK_STATISTICS_H_
