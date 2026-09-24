#ifndef MODERN_LEVELDB_TESTS_SUPPORT_MEMORY_FILE_SYSTEM_H_
#define MODERN_LEVELDB_TESTS_SUPPORT_MEMORY_FILE_SYSTEM_H_

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "modern_leveldb/base/bytes.h"
#include "modern_leveldb/base/result.h"
#include "platform/file_system.h"

namespace modern_leveldb::test_support {

// A single-threaded in-memory file system for unit tests. Written bytes are
// visible at once, and syncs only record that they happened. Every file
// operation is logged, and any logged operation can be made to fail without
// effect. Operations that no test needs return NotSupported.
class MemoryFileSystem : public FileSystem {
 public:
  // The operations so far, such as "append MANIFEST-000001" or
  // "rename 000001.dbtmp CURRENT". Files are named without their directory;
  // directory syncs name the directory.
  [[nodiscard]] const std::vector<std::string>& operations() const noexcept { return operations_; }

  // Makes the operation that will have this index in operations() fail.
  void FailOperation(std::size_t index, Error error);

  [[nodiscard]] std::optional<std::vector<std::byte>> Contents(
      const std::filesystem::path& path) const;
  void Write(const std::filesystem::path& path, std::vector<std::byte> contents);

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

 private:
  class SequentialMemoryFile;
  class WritableMemoryFile;

  // Logs the operation and returns the failure injected for it, if any.
  [[nodiscard]] Status Record(std::string operation);

  std::map<std::filesystem::path, std::vector<std::byte>> files_;
  std::vector<std::string> operations_;
  std::map<std::size_t, Error> failures_;
};

}  // namespace modern_leveldb::test_support

#endif  // MODERN_LEVELDB_TESTS_SUPPORT_MEMORY_FILE_SYSTEM_H_
