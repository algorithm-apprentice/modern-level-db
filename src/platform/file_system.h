#ifndef MODERN_LEVELDB_PLATFORM_FILE_SYSTEM_H_
#define MODERN_LEVELDB_PLATFORM_FILE_SYSTEM_H_

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <vector>

#include "modern_leveldb/base/bytes.h"
#include "modern_leveldb/base/result.h"

namespace modern_leveldb {

class SequentialFile {
 public:
  SequentialFile() = default;
  SequentialFile(const SequentialFile&) = delete;
  SequentialFile& operator=(const SequentialFile&) = delete;
  SequentialFile(SequentialFile&&) = delete;
  SequentialFile& operator=(SequentialFile&&) = delete;
  virtual ~SequentialFile() = default;

  [[nodiscard]] virtual Result<std::size_t> Read(MutableByteView output) = 0;
  [[nodiscard]] virtual Status Skip(std::uint64_t bytes) = 0;
};

class RandomAccessFile {
 public:
  RandomAccessFile() = default;
  RandomAccessFile(const RandomAccessFile&) = delete;
  RandomAccessFile& operator=(const RandomAccessFile&) = delete;
  RandomAccessFile(RandomAccessFile&&) = delete;
  RandomAccessFile& operator=(RandomAccessFile&&) = delete;
  virtual ~RandomAccessFile() = default;

  [[nodiscard]] virtual Result<std::size_t> Read(std::uint64_t offset,
                                                 MutableByteView output) const = 0;
};

class WritableFile {
 public:
  WritableFile() = default;
  WritableFile(const WritableFile&) = delete;
  WritableFile& operator=(const WritableFile&) = delete;
  WritableFile(WritableFile&&) = delete;
  WritableFile& operator=(WritableFile&&) = delete;
  virtual ~WritableFile() = default;

  [[nodiscard]] virtual Status Append(ByteView data) = 0;
  [[nodiscard]] virtual Status Flush() = 0;
  [[nodiscard]] virtual Status Sync() = 0;
  [[nodiscard]] virtual Status Close() = 0;
};

class FileLock {
 public:
  FileLock() = default;
  FileLock(const FileLock&) = delete;
  FileLock& operator=(const FileLock&) = delete;
  FileLock(FileLock&&) = delete;
  FileLock& operator=(FileLock&&) = delete;
  virtual ~FileLock() = 0;
};

inline FileLock::~FileLock() = default;

class FileSystem {
 public:
  FileSystem() = default;
  FileSystem(const FileSystem&) = delete;
  FileSystem& operator=(const FileSystem&) = delete;
  FileSystem(FileSystem&&) = delete;
  FileSystem& operator=(FileSystem&&) = delete;
  virtual ~FileSystem() = default;

  [[nodiscard]] virtual Result<std::unique_ptr<SequentialFile>> OpenSequential(
      const std::filesystem::path& path) = 0;
  [[nodiscard]] virtual Result<std::unique_ptr<RandomAccessFile>> OpenRandomAccess(
      const std::filesystem::path& path) = 0;
  [[nodiscard]] virtual Result<std::unique_ptr<WritableFile>> OpenWritable(
      const std::filesystem::path& path) = 0;
  [[nodiscard]] virtual Result<std::unique_ptr<WritableFile>> OpenAppendable(
      const std::filesystem::path& path) = 0;

  [[nodiscard]] virtual Result<bool> FileExists(const std::filesystem::path& path) const = 0;
  [[nodiscard]] virtual Result<std::vector<std::filesystem::path>> ListDirectory(
      const std::filesystem::path& path) const = 0;
  [[nodiscard]] virtual Result<std::uint64_t> FileSize(const std::filesystem::path& path) const = 0;

  [[nodiscard]] virtual Status CreateDirectory(const std::filesystem::path& path) = 0;
  [[nodiscard]] virtual Status RemoveFile(const std::filesystem::path& path) = 0;
  [[nodiscard]] virtual Status RemoveDirectory(const std::filesystem::path& path) = 0;
  [[nodiscard]] virtual Status RenameFile(const std::filesystem::path& source,
                                          const std::filesystem::path& destination) = 0;
  [[nodiscard]] virtual Status SyncDirectory(const std::filesystem::path& path) = 0;
  [[nodiscard]] virtual Result<std::unique_ptr<FileLock>> LockFile(
      const std::filesystem::path& path) = 0;
};

}  // namespace modern_leveldb

#endif  // MODERN_LEVELDB_PLATFORM_FILE_SYSTEM_H_
