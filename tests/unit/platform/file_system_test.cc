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

}  // namespace
}  // namespace modern_leveldb
