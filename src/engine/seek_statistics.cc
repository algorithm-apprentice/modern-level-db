#include "engine/seek_statistics.h"

#include <algorithm>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <set>
#include <utility>

#include "metadata/version_edit.h"

namespace modern_leveldb {
namespace {

// LevelDB allows a file one seek per 16 KiB, and at least 100.
constexpr std::uint64_t BytesPerSeek = 16384;
constexpr std::int64_t MinimumSeeks = 100;

std::int64_t InitialBudget(const FileMetadata& file) {
  return std::max(static_cast<std::int64_t>(file.file_size / BytesPerSeek), MinimumSeeks);
}

}  // namespace

std::function<std::uint64_t()> ReadSamplingPeriods(std::uint32_t seed) {
  // LevelDB's Random is the Park-Miller generator of std::minstd_rand0, and it
  // masks its seed to 31 bits.
  return [random = std::minstd_rand0(seed & 0x7fffffffU)]() mutable -> std::uint64_t {
    return random() % (2 * ReadBytesPeriod);
  };
}

bool SeekStatistics::Charge(const std::shared_ptr<const Version>& version,
                            const std::shared_ptr<const Version>& current,
                            const SeekCharge& charge) {
  std::int64_t& budget =
      budgets_.try_emplace(charge.file, InitialBudget(*charge.file)).first->second;
  --budget;
  if (budget > 0 || version != current || IsRecordedFor(current)) {
    return false;
  }
  SeekCompaction compaction{.level = charge.level, .file = charge.file};
  recorded_ = std::move(compaction);
  recorded_version_ = current;
  return true;
}

std::optional<SeekCompaction> SeekStatistics::FileToCompact(
    const std::shared_ptr<const Version>& current) const {
  if (!IsRecordedFor(current)) {
    return std::nullopt;
  }
  return recorded_;
}

void SeekStatistics::Retain(const Version& current) {
  std::set<const FileMetadata*> held;
  for (std::uint32_t level = 0; level < NumLevels; ++level) {
    for (const Version::File& file : current.files(level)) {
      held.insert(file.get());
    }
  }
  std::erase_if(budgets_, [&](const auto& entry) { return !held.contains(entry.first.get()); });
  const std::shared_ptr<const Version> recorded = recorded_version_.lock();
  if (recorded.get() != &current) {
    recorded_.reset();
    recorded_version_.reset();
  }
}

bool SeekStatistics::IsRecordedFor(const std::shared_ptr<const Version>& version) const {
  if (!recorded_.has_value()) {
    return false;
  }
  const std::shared_ptr<const Version> recorded = recorded_version_.lock();
  return recorded == version;
}

}  // namespace modern_leveldb
