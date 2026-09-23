#include "wal/wal_io.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "modern_leveldb/base/bytes.h"
#include "platform/posix_file_system.h"

namespace modern_leveldb {
namespace {

class WalTemporaryDirectory {
 public:
  WalTemporaryDirectory() {
    std::string pattern =
        (std::filesystem::temp_directory_path() / "modern-leveldb-wal-XXXXXX")
            .string();
    std::vector<char> writable(pattern.begin(), pattern.end());
    writable.push_back('\0');
    char* created = ::mkdtemp(writable.data());
    if (created == nullptr) {
      throw std::runtime_error("mkdtemp failed");
    }
    path_ = created;
  }

  WalTemporaryDirectory(const WalTemporaryDirectory&) = delete;
  WalTemporaryDirectory& operator=(const WalTemporaryDirectory&) = delete;

  ~WalTemporaryDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }

  const std::filesystem::path& path() const noexcept { return path_; }

 private:
  std::filesystem::path path_;
};

TEST(WalIoPosixTest, WritesSyncsClosesAndReopensARealFile) {
  WalTemporaryDirectory directory;
  PosixFileSystem file_system;
  const std::filesystem::path path = directory.path() / "000001.log";

  auto writable = file_system.OpenWritable(path);
  ASSERT_TRUE(writable.has_value());
  WalWriter writer(std::move(*writable));
  ASSERT_TRUE(writer.AddRecord(AsBytes("first")).has_value());
  const std::vector<std::byte> large(70'000, std::byte{0x5a});
  ASSERT_TRUE(writer.AddRecord(large).has_value());
  ASSERT_TRUE(writer.Sync().has_value());
  ASSERT_TRUE(writer.Close().has_value());

  auto sequential = file_system.OpenSequential(path);
  ASSERT_TRUE(sequential.has_value());
  WalReader reader(std::move(*sequential));

  auto first = reader.ReadNext();
  ASSERT_TRUE(first.has_value());
  ASSERT_TRUE(first->has_value());
  const auto* first_record = std::get_if<WalLogicalRecord>(&**first);
  ASSERT_NE(first_record, nullptr);
  EXPECT_EQ(AsStringView(first_record->data), "first");

  auto second = reader.ReadNext();
  ASSERT_TRUE(second.has_value());
  ASSERT_TRUE(second->has_value());
  const auto* second_record = std::get_if<WalLogicalRecord>(&**second);
  ASSERT_NE(second_record, nullptr);
  EXPECT_EQ(std::vector<std::byte>(second_record->data.begin(),
                                   second_record->data.end()),
            large);

  const auto eof = reader.ReadNext();
  ASSERT_TRUE(eof.has_value());
  EXPECT_FALSE(eof->has_value());
}

}  // namespace
}  // namespace modern_leveldb
