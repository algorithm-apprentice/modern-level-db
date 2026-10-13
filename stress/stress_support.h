#ifndef MODERN_LEVELDB_STRESS_STRESS_SUPPORT_H_
#define MODERN_LEVELDB_STRESS_STRESS_SUPPORT_H_

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <string_view>

#include "modern_leveldb/options.h"

namespace modern_leveldb::stress {

inline constexpr std::size_t StressWriteBufferSize = 64U * 1024U;
inline constexpr std::uint32_t StressBloomBitsPerKey = 10;

struct StressBuildIdentity {
    std::string revision;
    bool dirty = false;
};

struct PreparedStressPaths {
    std::filesystem::path database;
    std::filesystem::path output;
};

class CheckedOutput final {
public:
    explicit CheckedOutput(const std::filesystem::path& path);

    void Write(std::string_view text);

private:
    std::ofstream stream_;
};

[[nodiscard]] std::uint64_t ParseStressUnsigned(std::string_view text, std::string_view option);
[[nodiscard]] PreparedStressPaths PrepareStressPaths(const std::filesystem::path& database,
                                                     const std::filesystem::path& output);
[[nodiscard]] std::filesystem::path PrepareStressWorkspace(const std::filesystem::path& workspace);
[[nodiscard]] std::string StressCompressionName(Compression compression);
[[nodiscard]] Compression ParseStressCompression(std::string_view value);
[[nodiscard]] std::string StressHex(std::string_view bytes);
[[nodiscard]] std::string StableStressFingerprint(
    const std::map<std::string, std::string>& entries);
[[nodiscard]] std::string SanitizeStressMessage(std::string_view message);
void WriteStressText(const std::filesystem::path& path, std::string_view text);
[[nodiscard]] Options StressDatabaseOptions(Compression compression);

}  // namespace modern_leveldb::stress

#endif  // MODERN_LEVELDB_STRESS_STRESS_SUPPORT_H_
