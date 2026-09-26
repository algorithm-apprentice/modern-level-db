#include <gtest/gtest.h>

#include <charconv>
#include <fstream>
#include <string>

#include "support/database_model.h"
#include "support/temporary_directory.h"

namespace modern_leveldb {
namespace {

void MaterializeGolden(const std::filesystem::path& directory) {
  std::ifstream fixture(MODERN_LEVELDB_GOLDEN_DATABASE);
  if (!fixture) {
    throw std::runtime_error("cannot open the golden database fixture");
  }
  std::string name;
  while (std::getline(fixture, name)) {
    if (name.starts_with('#')) {
      continue;
    }
    std::string hex;
    if (name.empty() || std::filesystem::path(name).filename() != name ||
        !std::getline(fixture, hex) || hex.size() % 2 != 0) {
      throw std::runtime_error("malformed golden database fixture");
    }
    std::string bytes;
    for (std::size_t offset = 0; offset < hex.size(); offset += 2) {
      unsigned value = 0;
      const auto parsed = std::from_chars(hex.data() + offset, hex.data() + offset + 2, value, 16);
      if (parsed.ec != std::errc() || parsed.ptr != hex.data() + offset + 2) {
        throw std::runtime_error("malformed golden database hex");
      }
      bytes.push_back(static_cast<char>(value));
    }
    std::ofstream output(directory / name, std::ios::binary);
    output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
    output.close();
    if (!output) {
      throw std::runtime_error("cannot materialize golden database file");
    }
  }
}

TEST(LevelDbGoldenTest, OpensFrozenWalManifestAndCompressedTableImage) {
  test_support::TemporaryDirectory directory;
  MaterializeGolden(directory.path());
  test_support::ModernClient database(directory.path(), {});
  const test_support::Model expected{{std::string("binary\0key", 10), std::string("value\0", 6)},
                                     {"kept", std::string(256, 'x')}};
  EXPECT_EQ(database.Scan(false), test_support::ExpectedEntries(expected, false));
  EXPECT_FALSE(database.Read("deleted").has_value());
  database.Reopen();
  EXPECT_EQ(database.Scan(true), test_support::ExpectedEntries(expected, true));
}

}  // namespace
}  // namespace modern_leveldb
