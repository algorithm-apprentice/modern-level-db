#include "platform/file_system.h"

#include <gtest/gtest.h>

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <type_traits>
#include <utility>
#include <vector>

#include "modern_leveldb/base/bytes.h"
#include "modern_leveldb/base/result.h"
#include "support/memory_file_system.h"

namespace modern_leveldb {
namespace {

template <typename T>
constexpr bool IsOwnedPolymorphicInterface =
    std::is_abstract_v<T> && std::has_virtual_destructor_v<T> && !std::is_copy_constructible_v<T> &&
    !std::is_copy_assignable_v<T> && !std::is_move_constructible_v<T> &&
    !std::is_move_assignable_v<T>;

static_assert(IsOwnedPolymorphicInterface<SequentialFile>);
static_assert(IsOwnedPolymorphicInterface<RandomAccessFile>);
static_assert(IsOwnedPolymorphicInterface<WritableFile>);
static_assert(IsOwnedPolymorphicInterface<FileLock>);
static_assert(IsOwnedPolymorphicInterface<FileSystem>);

static_assert(std::same_as<decltype(std::declval<SequentialFile&>().Read(MutableByteView{})),
                           Result<std::size_t>>);
static_assert(std::same_as<decltype(std::declval<const RandomAccessFile&>().Read(
                               std::uint64_t{}, MutableByteView{})),
                           Result<std::size_t>>);
static_assert(std::same_as<decltype(std::declval<const RandomAccessFile&>().TryReadView(
                               std::uint64_t{}, std::size_t{})),
                           std::optional<ByteView>>);
static_assert(std::same_as<decltype(std::declval<WritableFile&>().Append(ByteView{})), Status>);
static_assert(std::same_as<decltype(std::declval<WritableFile&>().Flush()), Status>);
static_assert(std::same_as<decltype(std::declval<WritableFile&>().Sync()), Status>);
static_assert(std::same_as<decltype(std::declval<WritableFile&>().Close()), Status>);

using Path = std::filesystem::path;

static_assert(
    std::same_as<decltype(std::declval<FileSystem&>().OpenSequential(std::declval<const Path&>())),
                 Result<std::unique_ptr<SequentialFile>>>);
static_assert(std::same_as<
              decltype(std::declval<FileSystem&>().OpenRandomAccess(std::declval<const Path&>())),
              Result<std::unique_ptr<RandomAccessFile>>>);
static_assert(
    std::same_as<decltype(std::declval<FileSystem&>().OpenWritable(std::declval<const Path&>())),
                 Result<std::unique_ptr<WritableFile>>>);
static_assert(
    std::same_as<decltype(std::declval<FileSystem&>().OpenAppendable(std::declval<const Path&>())),
                 Result<std::unique_ptr<WritableFile>>>);
static_assert(std::same_as<
              decltype(std::declval<const FileSystem&>().FileExists(std::declval<const Path&>())),
              Result<bool>>);
static_assert(std::same_as<decltype(std::declval<const FileSystem&>().ListDirectory(
                               std::declval<const Path&>())),
                           Result<std::vector<Path>>>);
static_assert(
    std::same_as<decltype(std::declval<const FileSystem&>().FileSize(std::declval<const Path&>())),
                 Result<std::uint64_t>>);
static_assert(
    std::same_as<decltype(std::declval<FileSystem&>().CreateDirectory(std::declval<const Path&>())),
                 Status>);
static_assert(
    std::same_as<decltype(std::declval<FileSystem&>().RemoveFile(std::declval<const Path&>())),
                 Status>);
static_assert(
    std::same_as<decltype(std::declval<FileSystem&>().RemoveDirectory(std::declval<const Path&>())),
                 Status>);
static_assert(std::same_as<decltype(std::declval<FileSystem&>().RenameFile(
                               std::declval<const Path&>(), std::declval<const Path&>())),
                           Status>);
static_assert(
    std::same_as<decltype(std::declval<FileSystem&>().SyncDirectory(std::declval<const Path&>())),
                 Status>);
static_assert(
    std::same_as<decltype(std::declval<FileSystem&>().LockFile(std::declval<const Path&>())),
                 Result<std::unique_ptr<FileLock>>>);

TEST(FileSystemInterfaceTest, UsesFilesystemPathAsItsPathType) {
  EXPECT_TRUE((std::same_as<Path, std::filesystem::path>));
}

TEST(MemoryFileSystemTest, ReportsAndCanFailRecordedFileSizes) {
  test_support::MemoryFileSystem file_system;
  file_system.Write("db/data", std::vector<std::byte>(17));

  const Result<std::uint64_t> size = file_system.FileSize("db/data");

  ASSERT_TRUE(size.has_value());
  EXPECT_EQ(*size, 17U);
  ASSERT_FALSE(file_system.operations().empty());
  EXPECT_EQ(file_system.operations().back(), "size data");

  file_system.FailOperation(file_system.operations().size(), Error::Io("injected size failure"));
  const Result<std::uint64_t> failed = file_system.FileSize("db/data");
  ASSERT_FALSE(failed.has_value());
  EXPECT_EQ(failed.error().message(), "injected size failure");

  const Result<std::uint64_t> missing = file_system.FileSize("db/missing");
  ASSERT_FALSE(missing.has_value());
  EXPECT_EQ(missing.error().code(), ErrorCode::NotFound);
}

}  // namespace
}  // namespace modern_leveldb
