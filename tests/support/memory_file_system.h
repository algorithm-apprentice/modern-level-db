#ifndef MODERN_LEVELDB_TESTS_SUPPORT_MEMORY_FILE_SYSTEM_H_
#define MODERN_LEVELDB_TESTS_SUPPORT_MEMORY_FILE_SYSTEM_H_

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "modern_leveldb/base/bytes.h"
#include "modern_leveldb/base/result.h"
#include "platform/file_system.h"

namespace modern_leveldb::test_support {

// An in-memory file system for unit tests, safe to use from several threads.
// Written bytes are visible at once, and syncs only record that they happened.
// A random-access file reads the contents its path had when it was opened.
// Directories exist once they are created or added; files may be written into
// any directory. Locks conflict until they are released. Every file operation
// is logged, and any logged operation can be made to fail without effect or be
// intercepted by a hook. Operations that no test needs return NotSupported.
class MemoryFileSystem : public FileSystem {
 public:
  // The operations so far, such as "append MANIFEST-000001" or
  // "rename 000001.dbtmp CURRENT". Files are named without their directory;
  // directory syncs name the directory. Read it only while no other thread
  // uses the file system.
  [[nodiscard]] const std::vector<std::string>& operations() const noexcept { return operations_; }

  // Makes the operation that will have this index in operations() fail.
  void FailOperation(std::size_t index, Error error);

  // Calls the hook with each operation before it is logged, outside the file
  // system's lock, so that the hook may block, throw, or return an error. An
  // error fails the operation without effect, as FailOperation does; an
  // operation whose hook throws is neither logged nor performed.
  void SetOperationHook(std::function<Status(std::string_view operation)> hook);

  [[nodiscard]] std::optional<std::vector<std::byte>> Contents(
      const std::filesystem::path& path) const;
  void Write(const std::filesystem::path& path, std::vector<std::byte> contents);
  void Erase(const std::filesystem::path& path);
  void AddDirectory(const std::filesystem::path& path);

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
  class MemoryFileLock;
  class RandomAccessMemoryFile;
  class SequentialMemoryFile;
  class WritableMemoryFile;

  // Logs the operation after running the hook and returns the failure injected
  // for it, if any.
  [[nodiscard]] Status Record(std::string operation) const;

  mutable std::mutex mutex_;
  std::function<Status(std::string_view)> hook_;
  std::map<std::filesystem::path, std::vector<std::byte>> files_;
  std::set<std::filesystem::path> directories_;
  std::set<std::filesystem::path> locks_;
  mutable std::vector<std::string> operations_;
  std::map<std::size_t, Error> failures_;
};

}  // namespace modern_leveldb::test_support

#endif  // MODERN_LEVELDB_TESTS_SUPPORT_MEMORY_FILE_SYSTEM_H_
