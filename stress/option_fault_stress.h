#ifndef MODERN_LEVELDB_STRESS_OPTION_FAULT_STRESS_H_
#define MODERN_LEVELDB_STRESS_OPTION_FAULT_STRESS_H_

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

#include "modern_leveldb/options.h"
#include "support/database_model.h"

namespace modern_leveldb::stress {

struct OptionMatrixCell {
    Compression compression = Compression::None;
    bool allow_mmap_reads = true;
    std::uint32_t bloom_bits_per_key = 0;
    std::size_t write_buffer_size = 64U * 1024U;
    bool sync_wal_creation = true;

    friend bool operator==(const OptionMatrixCell&, const OptionMatrixCell&) = default;
};

struct OptionMatrixCellSummary {
    std::string id;
    std::uint64_t seed = 0;
    std::size_t operations = 0;
    std::size_t entries = 0;
    std::string fingerprint;

    friend bool operator==(const OptionMatrixCellSummary&,
                           const OptionMatrixCellSummary&) = default;
};

[[nodiscard]] std::vector<OptionMatrixCell> EnumerateOptionMatrix();
[[nodiscard]] std::string OptionMatrixCellId(const OptionMatrixCell& cell);
[[nodiscard]] std::uint64_t DeriveOptionMatrixSeed(std::uint64_t campaign_seed,
                                                   const OptionMatrixCell& cell);
[[nodiscard]] std::string StableModelFingerprint(const test_support::Model& model);
[[nodiscard]] Options OptionMatrixDatabaseOptions(const OptionMatrixCell& cell);
[[nodiscard]] OptionMatrixCellSummary RunOptionMatrixCell(
    const std::filesystem::path& database, const OptionMatrixCell& cell,
    std::uint64_t campaign_seed, std::size_t operations);

}  // namespace modern_leveldb::stress

#endif  // MODERN_LEVELDB_STRESS_OPTION_FAULT_STRESS_H_
