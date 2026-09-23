#include "metadata/filenames.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <system_error>

#include "modern_leveldb/base/result.h"

namespace modern_leveldb {
namespace {

constexpr std::size_t MinimumDigits = 6;
constexpr std::size_t MaximumDigits = std::numeric_limits<std::uint64_t>::digits10 + 1;
constexpr std::string_view DescriptorPrefix = "MANIFEST-";
constexpr std::string_view LogSuffix = ".log";
constexpr std::string_view TableSuffix = ".ldb";
constexpr std::string_view TempSuffix = ".dbtmp";

struct NumberedSuffix {
  std::string_view suffix;
  FileType type;
};

constexpr std::array<NumberedSuffix, 3> NumberedSuffixes{{
    {LogSuffix, FileType::Log},
    {TableSuffix, FileType::Table},
    {TempSuffix, FileType::Temp},
}};

// Canonical file-number spelling: decimal, zero-padded to at least six digits.
class NumberText final {
 public:
  explicit NumberText(std::uint64_t number) noexcept {
    std::array<char, MaximumDigits> digits{};
    const std::to_chars_result result =
        std::to_chars(digits.data(), digits.data() + digits.size(), number);
    const auto length = static_cast<std::size_t>(result.ptr - digits.data());
    const std::size_t padding = length < MinimumDigits ? MinimumDigits - length : 0;
    std::fill_n(text_.data(), padding, '0');
    std::copy_n(digits.data(), length, text_.data() + padding);
    size_ = padding + length;
  }

  [[nodiscard]] std::string_view view() const noexcept { return {text_.data(), size_}; }

 private:
  std::array<char, MaximumDigits> text_{};
  std::size_t size_ = 0;
};

std::optional<std::uint64_t> ParseCanonicalNumber(std::string_view digits) noexcept {
  std::uint64_t number = 0;
  const std::from_chars_result result =
      std::from_chars(digits.data(), digits.data() + digits.size(), number);
  if (result.ec != std::errc{} || number == 0 || NumberText(number).view() != digits) {
    return std::nullopt;
  }
  return number;
}

std::optional<ParsedFileName> ParseNumbered(FileType type, std::string_view digits) noexcept {
  return ParseCanonicalNumber(digits).transform(
      [type](std::uint64_t number) noexcept { return ParsedFileName{type, number}; });
}

std::string NumberedName(std::uint64_t number, std::string_view suffix) {
  std::string name(NumberText(number).view());
  name.append(suffix);
  return name;
}

std::string DescriptorName(std::uint64_t number) {
  std::string name(DescriptorPrefix);
  name.append(NumberText(number).view());
  return name;
}

}  // namespace

std::filesystem::path LogFileName(const std::filesystem::path& directory, std::uint64_t number) {
  assert(number > 0);
  return directory / NumberedName(number, LogSuffix);
}

std::filesystem::path TableFileName(const std::filesystem::path& directory,
                                    std::uint64_t number) {
  assert(number > 0);
  return directory / NumberedName(number, TableSuffix);
}

std::filesystem::path DescriptorFileName(const std::filesystem::path& directory,
                                         std::uint64_t number) {
  assert(number > 0);
  return directory / DescriptorName(number);
}

std::filesystem::path TempFileName(const std::filesystem::path& directory, std::uint64_t number) {
  assert(number > 0);
  return directory / NumberedName(number, TempSuffix);
}

std::filesystem::path CurrentFileName(const std::filesystem::path& directory) {
  return directory / "CURRENT";
}

std::filesystem::path LockFileName(const std::filesystem::path& directory) {
  return directory / "LOCK";
}

std::optional<ParsedFileName> ParseFileName(std::string_view file_name) noexcept {
  if (file_name == "CURRENT") {
    return ParsedFileName{FileType::Current, 0};
  }
  if (file_name == "LOCK") {
    return ParsedFileName{FileType::Lock, 0};
  }
  if (file_name.starts_with(DescriptorPrefix)) {
    return ParseNumbered(FileType::Descriptor, file_name.substr(DescriptorPrefix.size()));
  }
  for (const NumberedSuffix& entry : NumberedSuffixes) {
    if (file_name.ends_with(entry.suffix)) {
      file_name.remove_suffix(entry.suffix.size());
      return ParseNumbered(entry.type, file_name);
    }
  }
  return std::nullopt;
}

std::string CurrentFileContents(std::uint64_t descriptor_number) {
  assert(descriptor_number > 0);
  std::string contents = DescriptorName(descriptor_number);
  contents.push_back('\n');
  return contents;
}

Result<std::uint64_t> ParseCurrentFileContents(std::string_view contents) {
  if (!contents.ends_with('\n')) {
    return std::unexpected(Error::Corruption("CURRENT file does not end with a newline"));
  }
  contents.remove_suffix(1);
  const std::optional<ParsedFileName> parsed = ParseFileName(contents);
  if (!parsed.has_value() || parsed->type != FileType::Descriptor) {
    return std::unexpected(Error::Corruption("CURRENT file does not name a MANIFEST file"));
  }
  return parsed->number;
}

}  // namespace modern_leveldb
