#include "stress/option_fault_stress.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "stress/stress_support.h"

namespace modern_leveldb::stress {
namespace {

constexpr std::uint64_t FnvOffsetBasis = 14695981039346656037ULL;
constexpr std::uint64_t FnvPrime = 1099511628211ULL;

void HashByte(std::uint64_t& hash, std::uint8_t byte) {
    hash ^= byte;
    hash *= FnvPrime;
}

void HashUnsigned(std::uint64_t& hash, std::uint64_t value) {
    for (int shift = 0; shift < 64; shift += 8) {
        HashByte(hash, static_cast<std::uint8_t>(value >> shift));
    }
}

void HashBytes(std::uint64_t& hash, std::string_view bytes) {
    for (const unsigned char byte : bytes) {
        HashByte(hash, byte);
    }
}

std::string HashHex(std::uint64_t value) {
    constexpr std::string_view Digits = "0123456789abcdef";
    std::string result(16, '0');
    for (std::size_t index = result.size(); index > 0; --index) {
        result[index - 1] = Digits[value & 0x0fU];
        value >>= 4U;
    }
    return result;
}

std::string AccessName(bool allow_mmap_reads) {
    return allow_mmap_reads ? "map" : "copy";
}

std::string BloomName(std::uint32_t bits_per_key) {
    return std::to_string(bits_per_key);
}

std::string WalCreationName(bool sync_wal_creation) {
    return sync_wal_creation ? "sync" : "async";
}

}  // namespace

std::vector<OptionMatrixCell> EnumerateOptionMatrix() {
    constexpr std::array<Compression, 3> Compressions{
        Compression::None,
        Compression::Snappy,
        Compression::Zstd,
    };
    constexpr std::array<bool, 2> Booleans{true, false};
    constexpr std::array<std::uint32_t, 2> BloomBits{0, 10};
    constexpr std::array<std::size_t, 2> WriteBuffers{
        64U * 1024U,
        256U * 1024U,
    };

    std::vector<OptionMatrixCell> cells;
    cells.reserve(Compressions.size() * Booleans.size() * BloomBits.size() *
                  WriteBuffers.size() * Booleans.size());
    for (const Compression compression : Compressions) {
        for (const bool allow_mmap_reads : Booleans) {
            for (const std::uint32_t bloom_bits_per_key : BloomBits) {
                for (const std::size_t write_buffer_size : WriteBuffers) {
                    for (const bool sync_wal_creation : Booleans) {
                        cells.push_back({
                            compression,
                            allow_mmap_reads,
                            bloom_bits_per_key,
                            write_buffer_size,
                            sync_wal_creation,
                        });
                    }
                }
            }
        }
    }
    return cells;
}

std::string OptionMatrixCellId(const OptionMatrixCell& cell) {
    return "c-" + StressCompressionName(cell.compression) +
           "_access-" + AccessName(cell.allow_mmap_reads) +
           "_bloom-" + BloomName(cell.bloom_bits_per_key) +
           "_wb-" + std::to_string(cell.write_buffer_size) +
           "_wal-create-" + WalCreationName(cell.sync_wal_creation);
}

std::uint64_t DeriveOptionMatrixSeed(std::uint64_t campaign_seed,
                                     const OptionMatrixCell& cell) {
    std::uint64_t hash = FnvOffsetBasis;
    HashUnsigned(hash, campaign_seed);
    HashByte(hash, 0);
    HashBytes(hash, OptionMatrixCellId(cell));
    return hash;
}

std::string StableModelFingerprint(const test_support::Model& model) {
    std::uint64_t hash = FnvOffsetBasis;
    for (const auto& [key, value] : model) {
        HashUnsigned(hash, key.size());
        HashBytes(hash, key);
        HashUnsigned(hash, value.size());
        HashBytes(hash, value);
    }
    return HashHex(hash);
}

Options OptionMatrixDatabaseOptions(const OptionMatrixCell& cell) {
    Options options = StressDatabaseOptions(cell.compression);
    options.allow_mmap_reads = cell.allow_mmap_reads;
    options.bloom_bits_per_key =
        cell.bloom_bits_per_key == 0
            ? std::optional<std::uint32_t>()
            : std::optional<std::uint32_t>(cell.bloom_bits_per_key);
    options.write_buffer_size = cell.write_buffer_size;
    options.sync_wal_creation = cell.sync_wal_creation;
    return options;
}

OptionMatrixCellSummary RunOptionMatrixCell(const std::filesystem::path& database,
                                            const OptionMatrixCell& cell,
                                            std::uint64_t campaign_seed,
                                            std::size_t operations) {
    const std::uint64_t seed = DeriveOptionMatrixSeed(campaign_seed, cell);
    test_support::ModernClient client(database, OptionMatrixDatabaseOptions(cell));
    const test_support::Model model = test_support::RunModel(seed, operations, client);
    client.Reopen();
    test_support::RequireModelEqual(
        client.Scan(false), test_support::ExpectedEntries(model, false),
        "option matrix reopened forward scan differs from model");
    test_support::RequireModelEqual(
        client.Scan(true), test_support::ExpectedEntries(model, true),
        "option matrix reopened reverse scan differs from model");
    return {
        OptionMatrixCellId(cell),
        seed,
        operations,
        model.size(),
        StableModelFingerprint(model),
    };
}

}  // namespace modern_leveldb::stress
