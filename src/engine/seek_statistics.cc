#include "engine/seek_statistics.h"

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <functional>
#include <optional>
#include <random>

namespace modern_leveldb {

std::function<std::uint64_t()> ReadSamplingPeriods(std::uint32_t seed) {
    // A 31-bit Park-Miller generator gives reproducible sampling for a fixed seed.
    return [random = std::minstd_rand0(seed & 0x7fffffffU)]() mutable -> std::uint64_t {
        return random() % (2 * ReadBytesPeriod);
    };
}

bool SeekStatistics::Charge(const Version& version, const Version& current,
                            const SeekCharge& charge) {
    assert(charge.level < NumLevels);
    assert(std::ranges::any_of(version.files(charge.level),
                               [&](const Version::File& file) { return &file == charge.file; }));
    const FileMetadata& file = **charge.file;
    --file.allowed_seeks;
    if (file.allowed_seeks > 0 || &version != &current || IsRecordedFor(current)) {
        return false;
    }
    recorded_ = Recorded{
        .version = &current,
        .level = charge.level,
        .file = charge.file,
    };
    return true;
}

bool SeekStatistics::HasFileToCompact(const Version& current) const noexcept {
    return IsRecordedFor(current);
}

std::optional<SeekCompaction> SeekStatistics::FileToCompact(const Version& current) const {
    if (!IsRecordedFor(current)) {
        return std::nullopt;
    }
    return SeekCompaction{
        .level = recorded_->level,
        .file = *recorded_->file,
    };
}

void SeekStatistics::Retain(const Version& current) {
    if (!IsRecordedFor(current)) {
        recorded_.reset();
    }
}

bool SeekStatistics::IsRecordedFor(const Version& version) const noexcept {
    return recorded_.has_value() && recorded_->version == &version;
}

}  // namespace modern_leveldb
