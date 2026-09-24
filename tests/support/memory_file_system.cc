#include "support/memory_file_system.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <filesystem>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace modern_leveldb::test_support {
namespace {

std::string Name(const std::filesystem::path& path) { return path.filename().string(); }

std::unexpected<Error> Unsupported(const char* operation) {
  return std::unexpected(Error::NotSupported(std::string(operation) + " is not supported"));
}

}  // namespace

class MemoryFileSystem::SequentialMemoryFile final : public SequentialFile {
 public:
  SequentialMemoryFile(MemoryFileSystem& file_system, std::filesystem::path path)
      : file_system_(file_system), path_(std::move(path)) {}

  Result<std::size_t> Read(MutableByteView output) override {
    const Status recorded = file_system_.Record("read " + Name(path_));
    if (!recorded.has_value()) {
      return std::unexpected(recorded.error());
    }
    const std::vector<std::byte>& contents = file_system_.files_[path_];
    const std::size_t start = std::min(offset_, contents.size());
    const std::size_t count = std::min(output.size(), contents.size() - start);
    std::copy_n(contents.begin() + static_cast<std::ptrdiff_t>(start), count, output.begin());
    offset_ = start + count;
    return count;
  }

 private:
  MemoryFileSystem& file_system_;
  std::filesystem::path path_;
  std::size_t offset_ = 0;
};

class MemoryFileSystem::MemoryFileLock final : public FileLock {
 public:
  MemoryFileLock(std::set<std::filesystem::path>& locks, std::filesystem::path path)
      : locks_(locks), path_(std::move(path)) {}
  MemoryFileLock(const MemoryFileLock&) = delete;
  MemoryFileLock& operator=(const MemoryFileLock&) = delete;
  MemoryFileLock(MemoryFileLock&&) = delete;
  MemoryFileLock& operator=(MemoryFileLock&&) = delete;
  ~MemoryFileLock() override { locks_.erase(path_); }

 private:
  std::set<std::filesystem::path>& locks_;
  std::filesystem::path path_;
};

class MemoryFileSystem::RandomAccessMemoryFile final : public RandomAccessFile {
 public:
  RandomAccessMemoryFile(MemoryFileSystem& file_system, std::filesystem::path path,
                         std::vector<std::byte> contents)
      : file_system_(file_system), path_(std::move(path)), contents_(std::move(contents)) {}

  Result<std::size_t> Read(std::uint64_t offset, MutableByteView output) const override {
    const Status recorded = file_system_.Record("read " + Name(path_));
    if (!recorded.has_value()) {
      return std::unexpected(recorded.error());
    }
    const std::size_t start =
        static_cast<std::size_t>(std::min<std::uint64_t>(offset, contents_.size()));
    const std::size_t count = std::min(output.size(), contents_.size() - start);
    std::copy_n(contents_.begin() + static_cast<std::ptrdiff_t>(start), count, output.begin());
    return count;
  }

 private:
  MemoryFileSystem& file_system_;
  std::filesystem::path path_;
  std::vector<std::byte> contents_;
};

class MemoryFileSystem::WritableMemoryFile final : public WritableFile {
 public:
  WritableMemoryFile(MemoryFileSystem& file_system, std::filesystem::path path)
      : file_system_(file_system), path_(std::move(path)) {}

  Status Append(ByteView data) override {
    const Status recorded = file_system_.Record("append " + Name(path_));
    if (recorded.has_value()) {
      std::vector<std::byte>& contents = file_system_.files_[path_];
      contents.insert(contents.end(), data.begin(), data.end());
    }
    return recorded;
  }
  Status Flush() override { return file_system_.Record("flush " + Name(path_)); }
  Status Sync() override { return file_system_.Record("sync " + Name(path_)); }
  Status Close() override { return file_system_.Record("close " + Name(path_)); }

 private:
  MemoryFileSystem& file_system_;
  std::filesystem::path path_;
};

void MemoryFileSystem::FailOperation(std::size_t index, Error error) {
  failures_.insert_or_assign(index, std::move(error));
}

std::optional<std::vector<std::byte>> MemoryFileSystem::Contents(
    const std::filesystem::path& path) const {
  const auto found = files_.find(path);
  if (found == files_.end()) {
    return std::nullopt;
  }
  return found->second;
}

void MemoryFileSystem::Write(const std::filesystem::path& path, std::vector<std::byte> contents) {
  files_.insert_or_assign(path, std::move(contents));
}

Status MemoryFileSystem::Record(std::string operation) const {
  const std::size_t index = operations_.size();
  operations_.push_back(std::move(operation));
  const auto failure = failures_.find(index);
  if (failure != failures_.end()) {
    return std::unexpected(failure->second);
  }
  return {};
}

Result<std::unique_ptr<SequentialFile>> MemoryFileSystem::OpenSequential(
    const std::filesystem::path& path) {
  const Status recorded = Record("open_sequential " + Name(path));
  if (!recorded.has_value()) {
    return std::unexpected(recorded.error());
  }
  if (!files_.contains(path)) {
    return std::unexpected(Error::NotFound(path.string()));
  }
  return std::make_unique<SequentialMemoryFile>(*this, path);
}

Result<std::unique_ptr<RandomAccessFile>> MemoryFileSystem::OpenRandomAccess(
    const std::filesystem::path& path) {
  const Status recorded = Record("open_random_access " + Name(path));
  if (!recorded.has_value()) {
    return std::unexpected(recorded.error());
  }
  const auto found = files_.find(path);
  if (found == files_.end()) {
    return std::unexpected(Error::NotFound(path.string()));
  }
  return std::make_unique<RandomAccessMemoryFile>(*this, path, found->second);
}

Result<std::unique_ptr<WritableFile>> MemoryFileSystem::OpenWritable(
    const std::filesystem::path& path) {
  const Status recorded = Record("open_writable " + Name(path));
  if (!recorded.has_value()) {
    return std::unexpected(recorded.error());
  }
  files_.insert_or_assign(path, std::vector<std::byte>());
  return std::make_unique<WritableMemoryFile>(*this, path);
}

Result<std::unique_ptr<WritableFile>> MemoryFileSystem::OpenAppendable(
    const std::filesystem::path&) {
  return Unsupported("OpenAppendable");
}

Result<bool> MemoryFileSystem::FileExists(const std::filesystem::path& path) const {
  const Status recorded = Record("exists " + Name(path));
  if (!recorded.has_value()) {
    return std::unexpected(recorded.error());
  }
  return files_.contains(path) || directories_.contains(path);
}

Result<std::vector<std::filesystem::path>> MemoryFileSystem::ListDirectory(
    const std::filesystem::path& path) const {
  const Status recorded = Record("list " + path.string());
  if (!recorded.has_value()) {
    return std::unexpected(recorded.error());
  }
  if (!directories_.contains(path)) {
    return std::unexpected(Error::NotFound(path.string()));
  }
  std::vector<std::filesystem::path> names;
  for (const auto& entry : files_) {
    if (entry.first.parent_path() == path) {
      names.push_back(entry.first.filename());
    }
  }
  return names;
}

Result<std::uint64_t> MemoryFileSystem::FileSize(const std::filesystem::path&) const {
  return Unsupported("FileSize");
}

Status MemoryFileSystem::CreateDirectory(const std::filesystem::path& path) {
  const Status recorded = Record("create_directory " + path.string());
  if (recorded.has_value()) {
    directories_.insert(path);
  }
  return recorded;
}

Status MemoryFileSystem::RemoveFile(const std::filesystem::path& path) {
  const Status recorded = Record("remove " + Name(path));
  if (!recorded.has_value()) {
    return recorded;
  }
  if (files_.erase(path) == 0) {
    return std::unexpected(Error::NotFound(path.string()));
  }
  return {};
}

Status MemoryFileSystem::RemoveDirectory(const std::filesystem::path& path) {
  static_cast<void>(Record("remove_directory " + path.string()));
  return Unsupported("RemoveDirectory");
}

Status MemoryFileSystem::RenameFile(const std::filesystem::path& source,
                                    const std::filesystem::path& destination) {
  const Status recorded = Record("rename " + Name(source) + " " + Name(destination));
  if (!recorded.has_value()) {
    return recorded;
  }
  const auto found = files_.find(source);
  if (found == files_.end()) {
    return std::unexpected(Error::NotFound(source.string()));
  }
  std::vector<std::byte> contents = std::move(found->second);
  files_.erase(found);
  files_.insert_or_assign(destination, std::move(contents));
  return {};
}

Status MemoryFileSystem::SyncDirectory(const std::filesystem::path& path) {
  return Record("sync_directory " + path.string());
}

Result<std::unique_ptr<FileLock>> MemoryFileSystem::LockFile(const std::filesystem::path& path) {
  const Status recorded = Record("lock " + Name(path));
  if (!recorded.has_value()) {
    return std::unexpected(recorded.error());
  }
  if (!locks_.insert(path).second) {
    return std::unexpected(Error::Busy("lock already held: " + path.string()));
  }
  files_.try_emplace(path);
  return std::make_unique<MemoryFileLock>(locks_, path);
}

}  // namespace modern_leveldb::test_support
