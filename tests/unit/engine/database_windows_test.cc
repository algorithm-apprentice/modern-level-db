#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "engine/database.h"
#include "metadata/filenames.h"
#include "platform/windows_file_system.h"
#include "platform/windows_file_system_internal.h"
#include "support/temporary_directory.h"

namespace modern_leveldb {
namespace {

class FailNewLog final : public WindowsFileOperations {
 public:
  bool fail = false;
  HANDLE Open(const wchar_t* path, DWORD access, DWORD share, DWORD disposition,
              DWORD flags) const noexcept override {
    if (fail && disposition == CREATE_ALWAYS && std::wstring_view(path).ends_with(L".log")) {
      ::SetLastError(ERROR_DISK_FULL);
      return INVALID_HANDLE_VALUE;
    }
    return WindowsFileOperations::Open(path, access, share, disposition, flags);
  }
};

TEST(DatabaseWindowsTest, InjectedFilesystemKeepsItsOwnPolicyAndRecoversOrphanNumberCollisions) {
  test_support::TemporaryDirectory directory;
  const auto path = directory.path() / "db";
  auto operations = std::make_shared<FailNewLog>();
  WindowsFileSystem file_system(true, operations);
  DatabaseEngineOptions options;
  options.file_system = &file_system;
  options.create_if_missing = true;
  const std::string expected(80 * 1'024, 'x');
  {
    auto database = DatabaseEngine::Open(options, path);
    ASSERT_TRUE(database.has_value()) << database.error().ToString();
    EncodedWriteBatch batch;
    ASSERT_TRUE(batch.Put(AsBytes("key"), AsBytes(expected)).has_value());
    ASSERT_TRUE((*database)->Write(batch, true).has_value());
  }
  operations->fail = true;
  const auto failed = DatabaseEngine::Open(options, path);
  ASSERT_FALSE(failed.has_value());
  EXPECT_EQ(failed.error().code(), ErrorCode::Io);
  std::filesystem::path orphan;
  const auto names = file_system.ListDirectory(path);
  ASSERT_TRUE(names.has_value());
  for (const auto& name : *names) {
    const auto parsed = ParseNativeFileName(name);
    if (parsed.has_value() && parsed->type == FileType::Table) {
      ASSERT_TRUE(orphan.empty());
      orphan = path / name;
    }
  }
  ASSERT_FALSE(orphan.empty());
  auto held = file_system.OpenRandomAccess(orphan);
  ASSERT_TRUE(held.has_value());
  operations->fail = false;
  const auto collision = DatabaseEngine::Open(options, path);
  ASSERT_FALSE(collision.has_value());
  EXPECT_EQ(collision.error().code(), ErrorCode::Io);
  EXPECT_NE(collision.error().message().find("Win32 32"), std::string_view::npos);
  held->reset();
  auto recovered = DatabaseEngine::Open(options, path);
  ASSERT_TRUE(recovered.has_value()) << recovered.error().ToString();
  std::vector<std::byte> value;
  ASSERT_TRUE((*recovered)->Get(AsBytes("key"), value).value());
  EXPECT_EQ(AsStringView(value), expected);
}

}  // namespace
}  // namespace modern_leveldb
