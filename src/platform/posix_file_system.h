#ifndef MODERN_LEVELDB_PLATFORM_POSIX_FILE_SYSTEM_H_
#define MODERN_LEVELDB_PLATFORM_POSIX_FILE_SYSTEM_H_

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <vector>

#include "modern_leveldb/base/result.h"
#include "platform/file_system.h"

namespace modern_leveldb {

class PosixMmapBudget;

class PosixFileSystem final : public FileSystem {
 public:
  PosixFileSystem() noexcept;
  explicit PosixFileSystem(bool allow_mmap_reads) noexcept;
  // Uses an isolated mmap budget for tests.
  PosixFileSystem(std::size_t maximum_mappings, std::uint64_t maximum_mapped_bytes);
  explicit PosixFileSystem(std::shared_ptr<PosixMmapBudget> mmap_budget) noexcept;
  [[nodiscard]] static std::shared_ptr<PosixMmapBudget> NewMmapBudgetForTesting(
      std::size_t maximum_mappings, std::uint64_t maximum_mapped_bytes);

  [[nodiscard]] Result<std::unique_ptr<SequentialFile>> OpenSequential(
      const std::filesystem::path& path) override;
  [[nodiscard]] Result<std::unique_ptr<RandomAccessFile>> OpenRandomAccess(
      const std::filesystem::path& path,
      std::optional<std::uint64_t> expected_size = std::nullopt) override;
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

 private:
  std::shared_ptr<PosixMmapBudget> mmap_budget_;
};

}  // namespace modern_leveldb

#endif  // MODERN_LEVELDB_PLATFORM_POSIX_FILE_SYSTEM_H_
