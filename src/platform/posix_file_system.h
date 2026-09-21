#ifndef MODERN_LEVELDB_PLATFORM_POSIX_FILE_SYSTEM_H_
#define MODERN_LEVELDB_PLATFORM_POSIX_FILE_SYSTEM_H_

#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

#include "modern_leveldb/base/result.h"
#include "platform/file_system.h"

namespace modern_leveldb {

class PosixFileSystem final : public FileSystem {
 public:
  [[nodiscard]] Result<std::unique_ptr<SequentialFile>> OpenSequential(
      const std::filesystem::path& path) override;
  [[nodiscard]] Result<std::unique_ptr<RandomAccessFile>> OpenRandomAccess(
      const std::filesystem::path& path) override;
  [[nodiscard]] Result<std::unique_ptr<WritableFile>> OpenWritable(
      const std::filesystem::path& path) override;
  [[nodiscard]] Result<std::unique_ptr<WritableFile>> OpenAppendable(
      const std::filesystem::path& path) override;

  [[nodiscard]] Result<bool> FileExists(const std::filesystem::path& path) const override;
  [[nodiscard]] Result<std::vector<std::filesystem::path>> ListDirectory(
      const std::filesystem::path& path) const override;
  [[nodiscard]] Result<std::uint64_t> FileSize(const std::filesystem::path& path) const override;

  [[nodiscard]] Status CreateDirectory(const std::filesystem::path& path) override;
  [[nodiscard]] Status RemoveFile(const std::filesystem::path& path) override;
  [[nodiscard]] Status RemoveDirectory(const std::filesystem::path& path) override;
  [[nodiscard]] Status RenameFile(const std::filesystem::path& source,
                                  const std::filesystem::path& destination) override;
  [[nodiscard]] Status SyncDirectory(const std::filesystem::path& path) override;
  [[nodiscard]] Result<std::unique_ptr<FileLock>> LockFile(
      const std::filesystem::path& path) override;
};

}  // namespace modern_leveldb

#endif  // MODERN_LEVELDB_PLATFORM_POSIX_FILE_SYSTEM_H_
