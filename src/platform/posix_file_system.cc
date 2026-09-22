#include "platform/posix_file_system.h"

#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "modern_leveldb/base/bytes.h"
#include "modern_leveldb/base/result.h"
#include "platform/file_system.h"

namespace modern_leveldb {
namespace {

constexpr std::size_t WritableBufferSize = 64U * 1'024U;

std::string PathText(const std::filesystem::path& path) { return path.native(); }

std::optional<Error> ValidatePath(const std::filesystem::path& path) {
  const std::string native = path.native();
  if (native.empty()) {
    return Error::InvalidArgument("filesystem path is empty");
  }
  if (native.find('\0') != std::string::npos) {
    return Error::InvalidArgument("filesystem path contains an embedded null byte");
  }
  return std::nullopt;
}

std::string OperationMessage(const char* operation, const std::filesystem::path& path,
                             int error_number) {
  return std::string(operation) + " '" + PathText(path) +
         "': " + std::system_category().message(error_number);
}

Error FileError(const char* operation, const std::filesystem::path& path, int error_number) {
  const std::string message = OperationMessage(operation, path, error_number);
  if (error_number == ENOENT || error_number == ENOTDIR) {
    return Error::NotFound(message);
  }
  if (error_number == ENOSYS || error_number == ENOTSUP || error_number == EOPNOTSUPP) {
    return Error::NotSupported(message);
  }
  return Error::Io(message);
}

Error LockError(const std::filesystem::path& path, int error_number) {
  const std::string message = OperationMessage("lock", path, error_number);
  if (error_number == EACCES || error_number == EAGAIN) {
    return Error::Busy(message);
  }
  return FileError("lock", path, error_number);
}

class ScopedDescriptor {
 public:
  explicit ScopedDescriptor(int descriptor) noexcept : descriptor_(descriptor) {}

  ScopedDescriptor(const ScopedDescriptor&) = delete;
  ScopedDescriptor& operator=(const ScopedDescriptor&) = delete;

  ScopedDescriptor(ScopedDescriptor&& other) noexcept
      : descriptor_(std::exchange(other.descriptor_, -1)) {}

  ScopedDescriptor& operator=(ScopedDescriptor&& other) noexcept {
    if (this != &other) {
      if (descriptor_ >= 0) {
        (void)::close(descriptor_);
      }
      descriptor_ = std::exchange(other.descriptor_, -1);
    }
    return *this;
  }

  ~ScopedDescriptor() {
    if (descriptor_ >= 0) {
      (void)::close(descriptor_);
    }
  }

  [[nodiscard]] int get() const noexcept { return descriptor_; }
  [[nodiscard]] int Release() noexcept { return std::exchange(descriptor_, -1); }

 private:
  int descriptor_;
};

struct DirectoryCloser {
  void operator()(DIR* directory) const noexcept {
    if (directory != nullptr) {
      (void)::closedir(directory);
    }
  }
};

using ScopedDirectory = std::unique_ptr<DIR, DirectoryCloser>;

int OpenFlagsWithCloseOnExec(int flags) {
#if defined(O_CLOEXEC)
  return flags | O_CLOEXEC;
#else
  return flags;
#endif
}

Result<ScopedDescriptor> OpenDescriptor(const std::filesystem::path& path, int flags, mode_t mode) {
  if (const auto error = ValidatePath(path); error.has_value()) {
    return std::unexpected(*error);
  }

  int descriptor;
  do {
    descriptor = ::open(path.c_str(), OpenFlagsWithCloseOnExec(flags), mode);
  } while (descriptor < 0 && errno == EINTR);
  if (descriptor < 0) {
    return std::unexpected(FileError("open", path, errno));
  }

#if !defined(O_CLOEXEC)
  if (::fcntl(descriptor, F_SETFD, FD_CLOEXEC) == -1) {
    const int error_number = errno;
    ::close(descriptor);
    return std::unexpected(FileError("set close-on-exec", path, error_number));
  }
#endif
  return ScopedDescriptor(descriptor);
}

std::size_t MaximumIoSize(std::size_t requested) {
  return std::min(requested, static_cast<std::size_t>(std::numeric_limits<ssize_t>::max()));
}

Result<off_t> PosixOffset(std::uint64_t value, const char* operation) {
  constexpr auto MaximumOffset = static_cast<std::uint64_t>(std::numeric_limits<off_t>::max());
  if (value > MaximumOffset) {
    return std::unexpected(Error::InvalidArgument(std::string(operation) + " exceeds POSIX off_t"));
  }
  return static_cast<off_t>(value);
}

Status SyncDescriptor(int descriptor, const std::filesystem::path& path, bool data_only) {
#if defined(__APPLE__) && defined(F_FULLFSYNC)
  if (data_only) {
    int result;
    do {
      result = ::fcntl(descriptor, F_FULLFSYNC);
    } while (result == -1 && errno == EINTR);
    if (result == 0) {
      return {};
    }
    const int full_sync_error = errno;
    if (full_sync_error != EINVAL && full_sync_error != ENOTSUP && full_sync_error != EOPNOTSUPP) {
      return std::unexpected(FileError("full sync", path, full_sync_error));
    }
  }
#endif

  int result;
  do {
#if defined(__linux__)
    result = data_only ? ::fdatasync(descriptor) : ::fsync(descriptor);
#else
    (void)data_only;
    result = ::fsync(descriptor);
#endif
  } while (result == -1 && errno == EINTR);
  if (result == -1) {
    return std::unexpected(FileError("sync", path, errno));
  }
  return {};
}

Status CloseDescriptor(int descriptor, const std::filesystem::path& path) {
  if (::close(descriptor) == -1) {
    return std::unexpected(FileError("close", path, errno));
  }
  return {};
}

class PosixSequentialFile final : public SequentialFile {
 public:
  PosixSequentialFile(int descriptor, std::filesystem::path path)
      : descriptor_(descriptor), path_(std::move(path)) {}

  ~PosixSequentialFile() override { (void)::close(descriptor_); }

  Result<std::size_t> Read(MutableByteView output) override {
    if (output.empty()) {
      return 0U;
    }

    const std::size_t request = MaximumIoSize(output.size());
    ssize_t result;
    do {
      result = ::read(descriptor_, output.data(), request);
    } while (result < 0 && errno == EINTR);
    if (result < 0) {
      return std::unexpected(FileError("read", path_, errno));
    }
    return static_cast<std::size_t>(result);
  }

  Status Skip(std::uint64_t bytes) override {
    const auto offset = PosixOffset(bytes, "skip distance");
    if (!offset.has_value()) {
      return std::unexpected(offset.error());
    }

    off_t result;
    do {
      result = ::lseek(descriptor_, *offset, SEEK_CUR);
    } while (result == static_cast<off_t>(-1) && errno == EINTR);
    if (result == static_cast<off_t>(-1)) {
      return std::unexpected(FileError("skip", path_, errno));
    }
    return {};
  }

 private:
  const int descriptor_;
  const std::filesystem::path path_;
};

class PosixRandomAccessFile final : public RandomAccessFile {
 public:
  PosixRandomAccessFile(int descriptor, std::filesystem::path path)
      : descriptor_(descriptor), path_(std::move(path)) {}

  ~PosixRandomAccessFile() override { (void)::close(descriptor_); }

  Result<std::size_t> Read(std::uint64_t offset, MutableByteView output) const override {
    if (output.empty()) {
      return 0U;
    }
    const auto posix_offset = PosixOffset(offset, "read offset");
    if (!posix_offset.has_value()) {
      return std::unexpected(posix_offset.error());
    }

    const std::size_t request = MaximumIoSize(output.size());
    ssize_t result;
    do {
      result = ::pread(descriptor_, output.data(), request, *posix_offset);
    } while (result < 0 && errno == EINTR);
    if (result < 0) {
      return std::unexpected(FileError("positioned read", path_, errno));
    }
    return static_cast<std::size_t>(result);
  }

 private:
  const int descriptor_;
  const std::filesystem::path path_;
};

class PosixWritableFile final : public WritableFile {
 public:
  PosixWritableFile(int descriptor, std::filesystem::path path)
      : descriptor_(descriptor), path_(std::move(path)) {}

  ~PosixWritableFile() override {
    if (!close_attempted_) {
      close_attempted_ = true;
      const int descriptor = std::exchange(descriptor_, -1);
      if (descriptor >= 0) {
        if (!first_error_.has_value() && buffer_size_ > 0) {
          BestEffortWriteAll(descriptor, ByteView(buffer_.data(), buffer_size_));
        }
        (void)::close(descriptor);
      }
    }
  }

  Status Append(ByteView data) override {
    if (close_attempted_) {
      return std::unexpected(Error::InvalidArgument("append called after file close"));
    }
    if (first_error_.has_value()) {
      return std::unexpected(*first_error_);
    }
    if (data.empty()) {
      return {};
    }

    const std::size_t copy_size = std::min(data.size(), buffer_.size() - buffer_size_);
    std::memcpy(buffer_.data() + buffer_size_, data.data(), copy_size);
    buffer_size_ += copy_size;
    data = data.subspan(copy_size);
    if (data.empty()) {
      return {};
    }

    Status status = FlushBuffer(descriptor_);
    if (!status.has_value()) {
      return status;
    }
    if (data.size() < buffer_.size()) {
      std::memcpy(buffer_.data(), data.data(), data.size());
      buffer_size_ = data.size();
      return {};
    }
    return RememberError(WriteAll(descriptor_, data));
  }

  Status Flush() override {
    if (close_attempted_) {
      return std::unexpected(Error::InvalidArgument("flush called after file close"));
    }
    return FlushBuffer(descriptor_);
  }

  Status Sync() override {
    if (close_attempted_) {
      return std::unexpected(Error::InvalidArgument("sync called after file close"));
    }
    Status status = FlushBuffer(descriptor_);
    if (!status.has_value()) {
      return status;
    }
    return RememberError(SyncDescriptor(descriptor_, path_, true));
  }

  Status Close() override {
    if (close_attempted_) {
      if (first_error_.has_value()) {
        return std::unexpected(*first_error_);
      }
      return {};
    }
    close_attempted_ = true;

    ScopedDescriptor descriptor(std::exchange(descriptor_, -1));
    Status status = FlushBuffer(descriptor.get());
    const Status close_status = CloseDescriptor(descriptor.Release(), path_);
    if (!status.has_value()) {
      return status;
    }
    return RememberError(close_status);
  }

 private:
  Status RememberError(Status status) {
    if (!status.has_value() && !first_error_.has_value()) {
      first_error_ = status.error();
    }
    if (first_error_.has_value()) {
      return std::unexpected(*first_error_);
    }
    return {};
  }

  Status FlushBuffer(int descriptor) {
    if (first_error_.has_value()) {
      return std::unexpected(*first_error_);
    }
    if (buffer_size_ == 0) {
      return {};
    }

    const ByteView data(buffer_.data(), buffer_size_);
    buffer_size_ = 0;
    return RememberError(WriteAll(descriptor, data));
  }

  Status WriteAll(int descriptor, ByteView data) {
    while (!data.empty()) {
      const std::size_t request = MaximumIoSize(data.size());
      ssize_t written;
      do {
        written = ::write(descriptor, data.data(), request);
      } while (written < 0 && errno == EINTR);
      if (written < 0) {
        return std::unexpected(FileError("write", path_, errno));
      }
      if (written == 0) {
        return std::unexpected(Error::Io("write made no progress for '" + PathText(path_) + "'"));
      }
      data = data.subspan(static_cast<std::size_t>(written));
    }
    return {};
  }

  static void BestEffortWriteAll(int descriptor, ByteView data) noexcept {
    while (!data.empty()) {
      const std::size_t request = MaximumIoSize(data.size());
      ssize_t written;
      do {
        written = ::write(descriptor, data.data(), request);
      } while (written < 0 && errno == EINTR);
      if (written <= 0) {
        return;
      }
      data = data.subspan(static_cast<std::size_t>(written));
    }
  }

  std::array<std::byte, WritableBufferSize> buffer_{};
  std::size_t buffer_size_ = 0;
  int descriptor_;
  const std::filesystem::path path_;
  std::optional<Error> first_error_;
  bool close_attempted_ = false;
};

class LockRegistry {
 public:
  bool Reserve(const std::string& identity) {
    std::lock_guard lock(mutex_);
    return identities_.insert(identity).second;
  }

  void Release(const std::string& identity) {
    std::lock_guard lock(mutex_);
    identities_.erase(identity);
  }

 private:
  std::mutex mutex_;
  std::set<std::string> identities_;
};

LockRegistry& ProcessLockRegistry() {
  static LockRegistry registry;
  return registry;
}

class LockReservation {
 public:
  LockReservation(LockRegistry& registry, const std::string& identity)
      : registry_(registry), identity_(identity) {}

  LockReservation(const LockReservation&) = delete;
  LockReservation& operator=(const LockReservation&) = delete;

  ~LockReservation() {
    if (active_) {
      registry_.Release(identity_);
    }
  }

  void Commit() noexcept { active_ = false; }

 private:
  LockRegistry& registry_;
  const std::string& identity_;
  bool active_ = true;
};

int SetFileLock(int descriptor, short type) {
  struct flock lock{};
  lock.l_type = type;
  lock.l_whence = SEEK_SET;
  lock.l_start = 0;
  lock.l_len = 0;
  return ::fcntl(descriptor, F_SETLK, &lock);
}

class PosixFileLock final : public FileLock {
 public:
  PosixFileLock(int descriptor, std::string identity)
      : descriptor_(descriptor), identity_(std::move(identity)) {}

  ~PosixFileLock() override {
    (void)SetFileLock(descriptor_, F_UNLCK);
    (void)::close(descriptor_);
    ProcessLockRegistry().Release(identity_);
  }

 private:
  const int descriptor_;
  const std::string identity_;
};

Result<std::string> NormalizeLockIdentity(const std::filesystem::path& path) {
  std::error_code error;
  const std::filesystem::path canonical = std::filesystem::weakly_canonical(path, error);
  if (error) {
    return std::unexpected(
        Error::Io("normalize lock path '" + PathText(path) + "': " + error.message()));
  }
  return canonical.native();
}

}  // namespace

Result<std::unique_ptr<SequentialFile>> PosixFileSystem::OpenSequential(
    const std::filesystem::path& path) {
  auto descriptor = OpenDescriptor(path, O_RDONLY, 0);
  if (!descriptor.has_value()) {
    return std::unexpected(descriptor.error());
  }
  std::unique_ptr<SequentialFile> file =
      std::make_unique<PosixSequentialFile>(descriptor->get(), path);
  (void)descriptor->Release();
  return file;
}

Result<std::unique_ptr<RandomAccessFile>> PosixFileSystem::OpenRandomAccess(
    const std::filesystem::path& path) {
  auto descriptor = OpenDescriptor(path, O_RDONLY, 0);
  if (!descriptor.has_value()) {
    return std::unexpected(descriptor.error());
  }
  std::unique_ptr<RandomAccessFile> file =
      std::make_unique<PosixRandomAccessFile>(descriptor->get(), path);
  (void)descriptor->Release();
  return file;
}

Result<std::unique_ptr<WritableFile>> PosixFileSystem::OpenWritable(
    const std::filesystem::path& path) {
  auto descriptor = OpenDescriptor(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (!descriptor.has_value()) {
    return std::unexpected(descriptor.error());
  }
  std::unique_ptr<WritableFile> file = std::make_unique<PosixWritableFile>(descriptor->get(), path);
  (void)descriptor->Release();
  return file;
}

Result<std::unique_ptr<WritableFile>> PosixFileSystem::OpenAppendable(
    const std::filesystem::path& path) {
  auto descriptor = OpenDescriptor(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
  if (!descriptor.has_value()) {
    return std::unexpected(descriptor.error());
  }
  std::unique_ptr<WritableFile> file = std::make_unique<PosixWritableFile>(descriptor->get(), path);
  (void)descriptor->Release();
  return file;
}

Result<bool> PosixFileSystem::FileExists(const std::filesystem::path& path) const {
  if (const auto error = ValidatePath(path); error.has_value()) {
    return std::unexpected(*error);
  }

  struct stat file_status{};
  if (::stat(path.c_str(), &file_status) == 0) {
    return true;
  }
  if (errno == ENOENT || errno == ENOTDIR) {
    return false;
  }
  return std::unexpected(FileError("stat", path, errno));
}

Result<std::vector<std::filesystem::path>> PosixFileSystem::ListDirectory(
    const std::filesystem::path& path) const {
  if (const auto error = ValidatePath(path); error.has_value()) {
    return std::unexpected(*error);
  }

  ScopedDirectory directory(::opendir(path.c_str()));
  if (directory == nullptr) {
    return std::unexpected(FileError("open directory", path, errno));
  }

  std::vector<std::filesystem::path> children;
  int read_error = 0;
  while (true) {
    errno = 0;
    dirent* entry = ::readdir(directory.get());
    if (entry == nullptr) {
      read_error = errno;
      break;
    }
    const std::string name(entry->d_name);
    if (name != "." && name != "..") {
      children.emplace_back(name);
    }
  }
  DIR* raw_directory = directory.release();
  const int close_result = ::closedir(raw_directory);
  if (read_error != 0) {
    return std::unexpected(FileError("read directory", path, read_error));
  }
  if (close_result == -1) {
    return std::unexpected(FileError("close directory", path, errno));
  }
  return children;
}

Result<std::uint64_t> PosixFileSystem::FileSize(const std::filesystem::path& path) const {
  if (const auto error = ValidatePath(path); error.has_value()) {
    return std::unexpected(*error);
  }

  struct stat file_status{};
  if (::stat(path.c_str(), &file_status) == -1) {
    return std::unexpected(FileError("stat", path, errno));
  }
  if (file_status.st_size < 0) {
    return std::unexpected(Error::Io("negative file size for '" + PathText(path) + "'"));
  }
  return static_cast<std::uint64_t>(file_status.st_size);
}

Status PosixFileSystem::CreateDirectory(const std::filesystem::path& path) {
  if (const auto error = ValidatePath(path); error.has_value()) {
    return std::unexpected(*error);
  }
  if (::mkdir(path.c_str(), 0755) == 0) {
    return {};
  }
  const int create_error = errno;
  if (create_error == EEXIST) {
    struct stat file_status{};
    if (::stat(path.c_str(), &file_status) == -1) {
      return std::unexpected(FileError("stat existing directory", path, errno));
    }
    if (S_ISDIR(file_status.st_mode)) {
      return {};
    }
    return std::unexpected(Error::InvalidArgument(
        "directory path exists and is not a directory: '" + PathText(path) + "'"));
  }
  return std::unexpected(FileError("create directory", path, create_error));
}

Status PosixFileSystem::RemoveFile(const std::filesystem::path& path) {
  if (const auto error = ValidatePath(path); error.has_value()) {
    return std::unexpected(*error);
  }
  if (::unlink(path.c_str()) == -1) {
    return std::unexpected(FileError("remove file", path, errno));
  }
  return {};
}

Status PosixFileSystem::RemoveDirectory(const std::filesystem::path& path) {
  if (const auto error = ValidatePath(path); error.has_value()) {
    return std::unexpected(*error);
  }
  if (::rmdir(path.c_str()) == -1) {
    return std::unexpected(FileError("remove directory", path, errno));
  }
  return {};
}

Status PosixFileSystem::RenameFile(const std::filesystem::path& source,
                                   const std::filesystem::path& destination) {
  if (const auto error = ValidatePath(source); error.has_value()) {
    return std::unexpected(*error);
  }
  if (const auto error = ValidatePath(destination); error.has_value()) {
    return std::unexpected(*error);
  }
  if (::rename(source.c_str(), destination.c_str()) == -1) {
    return std::unexpected(FileError("rename", source, errno));
  }
  return {};
}

Status PosixFileSystem::SyncDirectory(const std::filesystem::path& path) {
#if defined(O_DIRECTORY)
  constexpr int DirectoryFlag = O_DIRECTORY;
#else
  constexpr int DirectoryFlag = 0;
#endif
  auto descriptor = OpenDescriptor(path, O_RDONLY | DirectoryFlag, 0);
  if (!descriptor.has_value()) {
    return std::unexpected(descriptor.error());
  }

  Status status = SyncDescriptor(descriptor->get(), path, false);
  const Status close_status = CloseDescriptor(descriptor->Release(), path);
  if (!status.has_value()) {
    return status;
  }
  return close_status;
}

Result<std::unique_ptr<FileLock>> PosixFileSystem::LockFile(const std::filesystem::path& path) {
  if (const auto error = ValidatePath(path); error.has_value()) {
    return std::unexpected(*error);
  }
  auto identity = NormalizeLockIdentity(path);
  if (!identity.has_value()) {
    return std::unexpected(identity.error());
  }

  LockRegistry& registry = ProcessLockRegistry();
  if (!registry.Reserve(*identity)) {
    return std::unexpected(Error::Busy("lock already held by this process: '" + *identity + "'"));
  }
  LockReservation reservation(registry, *identity);

  auto descriptor = OpenDescriptor(path, O_RDWR | O_CREAT, 0644);
  if (!descriptor.has_value()) {
    return std::unexpected(descriptor.error());
  }

  if (SetFileLock(descriptor->get(), F_WRLCK) == -1) {
    const int lock_error = errno;
    return std::unexpected(LockError(path, lock_error));
  }

  std::unique_ptr<FileLock> lock = std::make_unique<PosixFileLock>(descriptor->get(), *identity);
  (void)descriptor->Release();
  reservation.Commit();
  return lock;
}

}  // namespace modern_leveldb
