#ifndef MODERN_LEVELDB_METADATA_FILENAMES_H_
#define MODERN_LEVELDB_METADATA_FILENAMES_H_

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>

#include "modern_leveldb/base/result.h"

namespace modern_leveldb {

enum class FileType {
  Log,
  Lock,
  Table,
  Descriptor,
  Current,
  Temp,
};

struct ParsedFileName {
  FileType type;
  std::uint64_t number;
};

// Numbered file-name generators require a nonzero file number.
[[nodiscard]] std::filesystem::path LogFileName(const std::filesystem::path& directory,
                                                std::uint64_t number);
[[nodiscard]] std::filesystem::path TableFileName(const std::filesystem::path& directory,
                                                  std::uint64_t number);
[[nodiscard]] std::filesystem::path DescriptorFileName(const std::filesystem::path& directory,
                                                       std::uint64_t number);
[[nodiscard]] std::filesystem::path TempFileName(const std::filesystem::path& directory,
                                                 std::uint64_t number);
[[nodiscard]] std::filesystem::path CurrentFileName(const std::filesystem::path& directory);
[[nodiscard]] std::filesystem::path LockFileName(const std::filesystem::path& directory);

// Classifies one file-name component. Only the canonical names produced by the
// generators are recognized, so every parsed name regenerates to the same path.
[[nodiscard]] std::optional<ParsedFileName> ParseFileName(std::string_view file_name) noexcept;

[[nodiscard]] std::string CurrentFileContents(std::uint64_t descriptor_number);
[[nodiscard]] Result<std::uint64_t> ParseCurrentFileContents(std::string_view contents);

}  // namespace modern_leveldb

#endif  // MODERN_LEVELDB_METADATA_FILENAMES_H_
