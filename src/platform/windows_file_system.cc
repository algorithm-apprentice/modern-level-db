#include "platform/windows_file_system.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <exception>
#include <limits>
#include <memory>
#include <new>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "platform/mapped_read_limiter.h"
#include "platform/path.h"
#include "platform/windows_file_system_internal.h"

namespace modern_leveldb {
namespace {

constexpr std::size_t WritableBufferSize = 64U * 1'024U;
constexpr DWORD MetadataSharing = FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE;

std::shared_ptr<MappedReadLimiter> ProcessMmapLimiter() {
  static const auto limiter = std::make_shared<MappedReadLimiter>(sizeof(void*) >= 8 ? 1'000 : 0);
  return limiter;
}

std::string Utf8(std::wstring_view text) {
  if (text.empty() || text.size() > static_cast<std::size_t>(std::numeric_limits<int>::max())) {
    return {};
  }
  const int length = static_cast<int>(text.size());
  const int required = ::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), length,
                                             nullptr, 0, nullptr, nullptr);
  if (required == 0) {
    return {};
  }
  std::string result(static_cast<std::size_t>(required), '\0');
  if (::WideCharToMultiByte(CP_UTF8, WC_ERR_INVALID_CHARS, text.data(), length, result.data(),
                            required, nullptr, nullptr) == 0) {
    return {};
  }
  return result;
}

Error FileError(std::string_view operation, const std::filesystem::path& path, DWORD code) {
  std::string message(operation);
  message += " '" + PathUtf8(path).value_or("<unrepresentable>") + "' (Win32 " +
             std::to_string(code) + ")";
  std::array<wchar_t, 1'024> text{};
  const DWORD count =
      ::FormatMessageW(FORMAT_MESSAGE_FROM_SYSTEM | FORMAT_MESSAGE_IGNORE_INSERTS, nullptr, code, 0,
                       text.data(), static_cast<DWORD>(text.size()), nullptr);
  if (count != 0) {
    const std::string system_text = Utf8(std::wstring_view(text.data(), count));
    if (!system_text.empty()) {
      message += ": " + system_text;
    }
  }
  if (code == ERROR_FILE_NOT_FOUND || code == ERROR_PATH_NOT_FOUND) {
    return Error::NotFound(std::move(message));
  }
  if (code == ERROR_INVALID_NAME || code == ERROR_BAD_PATHNAME ||
      code == ERROR_FILENAME_EXCED_RANGE) {
    return Error::InvalidArgument(std::move(message));
  }
  if (code == ERROR_NOT_SUPPORTED || code == ERROR_CALL_NOT_IMPLEMENTED) {
    return Error::NotSupported(std::move(message));
  }
  return Error::Io(std::move(message));
}

bool DrivePath(std::wstring_view path) noexcept {
  return path.size() >= 7 && path.starts_with(L"\\\\?\\") &&
         ((path[4] >= L'A' && path[4] <= L'Z') || (path[4] >= L'a' && path[4] <= L'z')) &&
         path[5] == L':' && path[6] == L'\\';
}

bool ReservedDosComponent(std::wstring component) {
  component.resize(component.find(L'.') == std::wstring::npos ? component.size()
                                                              : component.find(L'.'));
  for (wchar_t& character : component) {
    if (character >= L'a' && character <= L'z') {
      character = static_cast<wchar_t>(character - L'a' + L'A');
    }
  }
  return component == L"CON" || component == L"PRN" || component == L"AUX" || component == L"NUL" ||
         (component.size() == 4 &&
          (component.starts_with(L"COM") || component.starts_with(L"LPT")) &&
          ((component[3] >= L'1' && component[3] <= L'9') || component[3] == L'\u00b9' ||
           component[3] == L'\u00b2' || component[3] == L'\u00b3'));
}

Result<std::filesystem::path> NativePath(const std::filesystem::path& path) {
  const std::wstring& native = path.native();
  if (native.empty() || native.find(L'\0') != std::wstring::npos) {
    return std::unexpected(Error::InvalidArgument("filesystem path is empty or contains a null"));
  }
  if (native.starts_with(L"\\\\.\\")) {
    return std::unexpected(Error::NotSupported("device namespace paths are not supported"));
  }
  if (native.starts_with(L"\\\\?\\")) {
    if (!DrivePath(native) && !native.starts_with(L"\\\\?\\UNC\\")) {
      return std::unexpected(Error::NotSupported("this extended path namespace is not supported"));
    }
    return path;
  }
  for (const auto& component_path : path) {
    const std::wstring& component = component_path.native();
    if (component.empty() || component == L"." || component == L".." || component == L"\\" ||
        component == L"/") {
      continue;
    }
    if (component.back() == L'.' || component.back() == L' ') {
      return std::unexpected(
          Error::InvalidArgument("ordinary DOS path components must not end in a dot or space"));
    }
    if (ReservedDosComponent(component)) {
      return std::unexpected(Error::NotSupported("DOS device names require an explicit file path"));
    }
  }
  DWORD required = ::GetFullPathNameW(native.c_str(), 0, nullptr, nullptr);
  if (required == 0) {
    return std::unexpected(FileError("resolve path", path, ::GetLastError()));
  }
  std::vector<wchar_t> full(required);
  while (true) {
    const DWORD size =
        ::GetFullPathNameW(native.c_str(), static_cast<DWORD>(full.size()), full.data(), nullptr);
    if (size == 0) {
      return std::unexpected(FileError("resolve path", path, ::GetLastError()));
    }
    if (size < full.size()) {
      std::wstring result(full.data(), size);
      if (result.starts_with(L"\\\\")) {
        return std::filesystem::path(L"\\\\?\\UNC\\" + result.substr(2));
      }
      return std::filesystem::path(L"\\\\?\\" + result);
    }
    full.resize(size);
  }
}

class ScopedHandle final {
 public:
  ScopedHandle(HANDLE handle, std::shared_ptr<WindowsFileOperations> operations) noexcept
      : handle_(handle), operations_(std::move(operations)) {}
  ScopedHandle(const ScopedHandle&) = delete;
  ScopedHandle& operator=(const ScopedHandle&) = delete;
  ScopedHandle(ScopedHandle&& other) noexcept
      : handle_(std::exchange(other.handle_, INVALID_HANDLE_VALUE)),
        operations_(std::move(other.operations_)) {}
  ScopedHandle& operator=(ScopedHandle&&) = delete;
  ~ScopedHandle() {
    if (valid()) {
      static_cast<void>(operations_->Close(handle_));
    }
  }
  [[nodiscard]] HANDLE get() const noexcept { return handle_; }
  [[nodiscard]] bool valid() const noexcept {
    return handle_ != INVALID_HANDLE_VALUE && handle_ != nullptr;
  }
  [[nodiscard]] BOOL Close() noexcept {
    assert(valid());
    return operations_->Close(std::exchange(handle_, INVALID_HANDLE_VALUE));
  }

 private:
  HANDLE handle_;
  std::shared_ptr<WindowsFileOperations> operations_;
};

Result<ScopedHandle> OpenHandle(const std::filesystem::path& path, DWORD access, DWORD share,
                                DWORD disposition, DWORD flags,
                                const std::shared_ptr<WindowsFileOperations>& operations,
                                bool lock_contention = false) {
  const HANDLE opened = operations->Open(path.c_str(), access, share, disposition, flags);
  if (opened == INVALID_HANDLE_VALUE) {
    const DWORD code = ::GetLastError();
    const Error error = FileError("open", path, code);
    if (lock_contention && code == ERROR_SHARING_VIOLATION) {
      return std::unexpected(Error::Busy(std::string(error.message())));
    }
    return std::unexpected(error);
  }
  return ScopedHandle(opened, operations);
}

Status CheckKind(HANDLE file, const std::filesystem::path& path, bool directory,
                 const WindowsFileOperations& operations) {
  ::SetLastError(ERROR_SUCCESS);
  const DWORD kind = ::GetFileType(file);
  if (kind == FILE_TYPE_UNKNOWN && ::GetLastError() != ERROR_SUCCESS) {
    return std::unexpected(FileError("file type", path, ::GetLastError()));
  }
  if (kind != FILE_TYPE_DISK) {
    return std::unexpected(Error::NotSupported("only disk files and directories are supported"));
  }
  BY_HANDLE_FILE_INFORMATION information{};
  if (!operations.Inspect(file, &information)) {
    return std::unexpected(FileError("file information", path, ::GetLastError()));
  }
  if (((information.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) != directory) {
    return std::unexpected(Error::InvalidArgument("path has the wrong file kind"));
  }
  return {};
}

Result<ScopedHandle> OpenDirectory(const std::filesystem::path& path,
                                   const std::shared_ptr<WindowsFileOperations>& operations) {
  auto file = OpenHandle(path, FILE_READ_ATTRIBUTES, MetadataSharing, OPEN_EXISTING,
                         FILE_FLAG_BACKUP_SEMANTICS, operations);
  if (!file.has_value()) {
    return file;
  }
  const Status valid = CheckKind(file->get(), path, true, *operations);
  if (!valid.has_value()) {
    return std::unexpected(valid.error());
  }
  return file;
}

Status CloseHandle(ScopedHandle& handle, const std::filesystem::path& path) {
  if (!handle.Close()) {
    return std::unexpected(FileError("close", path, ::GetLastError()));
  }
  return {};
}

class WindowsSequentialFile final : public SequentialFile {
 public:
  WindowsSequentialFile(ScopedHandle file, std::filesystem::path path,
                        std::shared_ptr<WindowsFileOperations> operations)
      : file_(std::move(file)), path_(std::move(path)), operations_(std::move(operations)) {}
  Result<std::size_t> Read(MutableByteView output) override {
    if (output.empty()) {
      return 0U;
    }
    DWORD read = 0;
    if (!operations_->Read(file_.get(), output.data(), WindowsIoRequestSize(output.size()), &read,
                           nullptr)) {
      const DWORD error = ::GetLastError();
      if (error == ERROR_HANDLE_EOF) {
        return 0U;
      }
      return std::unexpected(FileError("read", path_, error));
    }
    return static_cast<std::size_t>(read);
  }

 private:
  ScopedHandle file_;
  const std::filesystem::path path_;
  const std::shared_ptr<WindowsFileOperations> operations_;
};

class WindowsRandomAccessFile final : public RandomAccessFile {
 public:
  WindowsRandomAccessFile(ScopedHandle file, std::filesystem::path path,
                          std::shared_ptr<WindowsFileOperations> operations)
      : file_(std::move(file)), path_(std::move(path)), operations_(std::move(operations)) {}
  Result<std::size_t> Read(std::uint64_t offset, MutableByteView output) const override {
    if (output.empty()) {
      return 0U;
    }
    constexpr auto MaximumOffset =
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    const DWORD request = WindowsIoRequestSize(output.size());
    if (offset > MaximumOffset || request > MaximumOffset - offset) {
      return std::unexpected(Error::InvalidArgument("read range exceeds signed 64-bit offsets"));
    }
    ScopedHandle event(operations_->Event(), operations_);
    if (!event.valid()) {
      return std::unexpected(FileError("create read event", path_, ::GetLastError()));
    }
    OVERLAPPED overlapped{};
    overlapped.Offset = static_cast<DWORD>(offset);
    overlapped.OffsetHigh = static_cast<DWORD>(offset >> 32U);
    overlapped.hEvent = event.get();
    if (!operations_->Read(file_.get(), output.data(), request, nullptr, &overlapped)) {
      const DWORD error = ::GetLastError();
      if (error == ERROR_HANDLE_EOF) {
        return 0U;
      }
      if (error != ERROR_IO_PENDING) {
        return std::unexpected(FileError("positioned read", path_, error));
      }
    }
    DWORD read = 0;
    if (!operations_->Complete(file_.get(), &overlapped, &read)) {
      const DWORD error = ::GetLastError();
      // Even a failed wait must not let the kernel retain this stack/buffer.
      if (!HasOverlappedIoCompleted(&overlapped)) {
        static_cast<void>(::CancelIoEx(file_.get(), &overlapped));
        while (!HasOverlappedIoCompleted(&overlapped)) {
          DWORD ignored = 0;
          static_cast<void>(::GetOverlappedResult(file_.get(), &overlapped, &ignored, TRUE));
        }
      }
      if (error == ERROR_HANDLE_EOF) {
        return 0U;
      }
      return std::unexpected(FileError("complete positioned read", path_, error));
    }
    return static_cast<std::size_t>(read);
  }

 private:
  ScopedHandle file_;
  const std::filesystem::path path_;
  const std::shared_ptr<WindowsFileOperations> operations_;
};

class PendingWindowsMapping final {
 public:
  PendingWindowsMapping(std::shared_ptr<MappedReadLimiter> limiter,
                        std::shared_ptr<WindowsFileOperations> operations) noexcept
      : limiter_(std::move(limiter)), operations_(std::move(operations)) {}
  PendingWindowsMapping(const PendingWindowsMapping&) = delete;
  PendingWindowsMapping& operator=(const PendingWindowsMapping&) = delete;
  ~PendingWindowsMapping() {
    if (active_) {
      if (view_ != nullptr && !operations_->Unmap(view_)) {
        std::terminate();
      }
      limiter_->Release();
    }
  }
  void SetView(const void* view) noexcept { view_ = view; }
  void Commit() noexcept { active_ = false; }

 private:
  const std::shared_ptr<MappedReadLimiter> limiter_;
  const std::shared_ptr<WindowsFileOperations> operations_;
  const void* view_ = nullptr;
  bool active_ = true;
};

class WindowsMappedRandomAccessFile final : public RandomAccessFile {
 public:
  WindowsMappedRandomAccessFile(const void* view, std::size_t length,
                                std::shared_ptr<MappedReadLimiter> limiter,
                                std::shared_ptr<WindowsFileOperations> operations) noexcept
      : view_(static_cast<const std::byte*>(view)),
        length_(length),
        limiter_(std::move(limiter)),
        operations_(std::move(operations)) {}
  ~WindowsMappedRandomAccessFile() override {
    if (!operations_->Unmap(view_)) {
      std::terminate();
    }
    limiter_->Release();
  }
  Result<std::size_t> Read(std::uint64_t offset, MutableByteView output) const override {
    if (output.empty()) {
      return 0U;
    }
    constexpr auto MaximumOffset =
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    const DWORD request = WindowsIoRequestSize(output.size());
    if (offset > MaximumOffset || request > MaximumOffset - offset) {
      return std::unexpected(Error::InvalidArgument("read range exceeds signed 64-bit offsets"));
    }
    if (offset >= length_) {
      return 0U;
    }
    const auto start = static_cast<std::size_t>(offset);
    const auto count = std::min(static_cast<std::size_t>(request), length_ - start);
    std::memcpy(output.data(), view_ + start, count);
    return count;
  }
  std::optional<ByteView> TryReadView(std::uint64_t offset,
                                      std::size_t size) const noexcept override {
    if (offset > length_) {
      return std::nullopt;
    }
    const auto start = static_cast<std::size_t>(offset);
    if (size > length_ - start) {
      return std::nullopt;
    }
    return ByteView(view_ + start, size);
  }

 private:
  const std::byte* const view_;
  const std::size_t length_;
  const std::shared_ptr<MappedReadLimiter> limiter_;
  const std::shared_ptr<WindowsFileOperations> operations_;
};

class WindowsWritableFile final : public WritableFile {
 public:
  WindowsWritableFile(ScopedHandle file, std::filesystem::path path,
                      std::shared_ptr<WindowsFileOperations> operations)
      : file_(std::move(file)), path_(std::move(path)), operations_(std::move(operations)) {}
  ~WindowsWritableFile() override {
    if (file_.valid() && !first_error_.has_value()) {
      std::size_t remaining = buffer_size_;
      const std::byte* data = buffer_.data();
      while (remaining != 0) {
        DWORD written = 0;
        if (!operations_->Write(file_.get(), data, WindowsIoRequestSize(remaining), &written) ||
            written == 0) {
          break;
        }
        data += written;
        remaining -= written;
      }
    }
  }
  Status Append(ByteView data) override {
    if (first_error_.has_value()) {
      return std::unexpected(*first_error_);
    }
    const std::size_t copied = std::min(data.size(), buffer_.size() - buffer_size_);
    if (copied != 0) {
      std::memcpy(buffer_.data() + buffer_size_, data.data(), copied);
    }
    buffer_size_ += copied;
    data = data.subspan(copied);
    if (data.empty()) {
      return {};
    }
    const Status flushed = Flush();
    if (!flushed.has_value()) {
      return flushed;
    }
    if (data.size() < buffer_.size()) {
      std::memcpy(buffer_.data(), data.data(), data.size());
      buffer_size_ = data.size();
      return {};
    }
    return Remember(WriteAll(data));
  }
  Status Flush() override {
    if (first_error_.has_value()) {
      return std::unexpected(*first_error_);
    }
    const ByteView bytes(buffer_.data(), buffer_size_);
    buffer_size_ = 0;
    return Remember(WriteAll(bytes));
  }
  Status Sync() override {
    const Status flushed = Flush();
    if (!flushed.has_value()) {
      return flushed;
    }
    if (!operations_->Sync(file_.get())) {
      return Remember(std::unexpected(FileError("sync", path_, ::GetLastError())));
    }
    return {};
  }
  Status Close() override {
    const Status flushed = Flush();
    const Status closed = CloseHandle(file_, path_);
    if (!flushed.has_value()) {
      return flushed;
    }
    return Remember(closed);
  }

 private:
  Status Remember(Status status) {
    if (!status.has_value() && !first_error_.has_value()) {
      first_error_ = status.error();
    }
    if (first_error_.has_value()) {
      return std::unexpected(*first_error_);
    }
    return {};
  }
  Status WriteAll(ByteView data) {
    while (!data.empty()) {
      DWORD written = 0;
      if (!operations_->Write(file_.get(), data.data(), WindowsIoRequestSize(data.size()),
                              &written)) {
        return std::unexpected(FileError("write", path_, ::GetLastError()));
      }
      if (written == 0) {
        return std::unexpected(Error::Io("write made no progress"));
      }
      data = data.subspan(written);
    }
    return {};
  }

  ScopedHandle file_;
  const std::filesystem::path path_;
  const std::shared_ptr<WindowsFileOperations> operations_;
  std::array<std::byte, WritableBufferSize> buffer_{};
  std::size_t buffer_size_ = 0;
  std::optional<Error> first_error_;
};

class WindowsFileLock final : public FileLock {
 public:
  explicit WindowsFileLock(ScopedHandle handle) noexcept : handle_(std::move(handle)) {}
  ~WindowsFileLock() override {
    OVERLAPPED overlapped{};
    static_cast<void>(::UnlockFileEx(handle_.get(), 0, MAXDWORD, MAXDWORD, &overlapped));
  }

 private:
  ScopedHandle handle_;
};

Result<std::unique_ptr<WritableFile>> OpenWriter(
    const std::filesystem::path& path, bool append,
    const std::shared_ptr<WindowsFileOperations>& operations) {
  auto native = NativePath(path);
  if (!native.has_value()) {
    return std::unexpected(native.error());
  }
  auto handle = OpenHandle(*native, GENERIC_WRITE, 0, append ? OPEN_ALWAYS : CREATE_ALWAYS,
                           FILE_ATTRIBUTE_NORMAL, operations);
  if (!handle.has_value()) {
    return std::unexpected(handle.error());
  }
  const Status valid = CheckKind(handle->get(), path, false, *operations);
  if (!valid.has_value()) {
    return std::unexpected(valid.error());
  }
  if (append) {
    LARGE_INTEGER zero{};
    if (!::SetFilePointerEx(handle->get(), zero, nullptr, FILE_END)) {
      return std::unexpected(FileError("seek to append", path, ::GetLastError()));
    }
  }
  return std::make_unique<WindowsWritableFile>(std::move(*handle), path, operations);
}

}  // namespace

WindowsFileSystem::WindowsFileSystem(bool allow_weak_namespace_durability, bool allow_mmap_reads)
    : WindowsFileSystem(allow_weak_namespace_durability, std::make_shared<WindowsFileOperations>(),
                        allow_mmap_reads ? ProcessMmapLimiter() : nullptr) {}

WindowsFileSystem::WindowsFileSystem(bool allow_weak_namespace_durability,
                                     std::shared_ptr<WindowsFileOperations> operations)
    : WindowsFileSystem(allow_weak_namespace_durability, std::move(operations),
                        ProcessMmapLimiter()) {}

WindowsFileSystem::WindowsFileSystem(bool allow_weak_namespace_durability,
                                     std::shared_ptr<WindowsFileOperations> operations,
                                     std::shared_ptr<MappedReadLimiter> mmap_limiter)
    : allow_weak_namespace_durability_(allow_weak_namespace_durability),
      operations_(std::move(operations)),
      mmap_limiter_(std::move(mmap_limiter)) {
  assert(operations_ != nullptr);
}

std::shared_ptr<MappedReadLimiter> WindowsFileSystem::NewMmapBudgetForTesting(
    std::size_t maximum_mappings) {
  if (maximum_mappings > static_cast<std::size_t>(std::numeric_limits<std::ptrdiff_t>::max())) {
    throw std::bad_array_new_length();
  }
  return std::make_shared<MappedReadLimiter>(static_cast<std::ptrdiff_t>(maximum_mappings));
}

Result<std::filesystem::path> WindowsFileSystem::PrepareDatabaseDirectory(
    const std::filesystem::path& directory) const {
  auto native = NativePath(directory);
  if (!native.has_value()) {
    return std::unexpected(native.error());
  }
  if (!allow_weak_namespace_durability_) {
    return std::unexpected(
        Error::NotSupported("Windows requires explicit consent to weak namespace durability"));
  }
  if (!DrivePath(native->native())) {
    return std::unexpected(Error::NotSupported("Windows databases require local fixed NTFS"));
  }
  while (native->native().size() > 7 && !native->has_filename()) {
    *native = native->parent_path();
  }
  bool missing = false;
  const DWORD attributes = ::GetFileAttributesW(native->c_str());
  if (attributes == INVALID_FILE_ATTRIBUTES) {
    const DWORD error = ::GetLastError();
    if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND) {
      return std::unexpected(FileError("database attributes", directory, error));
    }
    missing = true;
  } else if ((attributes & FILE_ATTRIBUTE_DIRECTORY) == 0) {
    return std::unexpected(Error::InvalidArgument("database path is not a directory"));
  }
  auto parent = OpenDirectory(missing ? native->parent_path() : *native, operations_);
  if (!parent.has_value()) {
    return std::unexpected(parent.error());
  }
  const DWORD required = ::GetFinalPathNameByHandleW(parent->get(), nullptr, 0, VOLUME_NAME_DOS);
  if (required == 0) {
    return std::unexpected(FileError("resolve database directory", directory, ::GetLastError()));
  }
  std::vector<wchar_t> resolved(static_cast<std::size_t>(required) + 1);
  const DWORD length = ::GetFinalPathNameByHandleW(
      parent->get(), resolved.data(), static_cast<DWORD>(resolved.size()), VOLUME_NAME_DOS);
  if (length == 0 || length >= resolved.size()) {
    const DWORD error = length == 0 ? ::GetLastError() : ERROR_INSUFFICIENT_BUFFER;
    return std::unexpected(FileError("resolve database directory", directory, error));
  }
  std::filesystem::path final(std::wstring(resolved.data(), length));
  if (!DrivePath(final.native())) {
    return std::unexpected(Error::NotSupported("resolved database volume is not local NTFS"));
  }
  std::vector<wchar_t> volume(std::max<std::size_t>(final.native().size() + 1, MAX_PATH + 1U));
  if (!::GetVolumePathNameW(final.c_str(), volume.data(), static_cast<DWORD>(volume.size()))) {
    return std::unexpected(FileError("database volume", directory, ::GetLastError()));
  }
  const UINT drive_type = operations_->DriveType(volume.data());
  if (drive_type == DRIVE_UNKNOWN || drive_type == DRIVE_NO_ROOT_DIR) {
    return std::unexpected(Error::Io("could not determine the database volume type"));
  }
  if (drive_type != DRIVE_FIXED) {
    return std::unexpected(Error::NotSupported("Windows databases require a fixed volume"));
  }
  std::array<wchar_t, 32> file_system{};
  if (!operations_->VolumeInformation(volume.data(), file_system.data(),
                                      static_cast<DWORD>(file_system.size()))) {
    return std::unexpected(FileError("database filesystem", directory, ::GetLastError()));
  }
  if (::CompareStringOrdinal(file_system.data(), -1, L"NTFS", -1, TRUE) != CSTR_EQUAL) {
    return std::unexpected(Error::NotSupported("Windows databases require NTFS"));
  }
  const Status closed = CloseHandle(*parent, directory);
  if (!closed.has_value()) {
    return std::unexpected(closed.error());
  }
  return missing ? final / native->filename() : final;
}

Result<std::unique_ptr<SequentialFile>> WindowsFileSystem::OpenSequential(
    const std::filesystem::path& path) {
  auto native = NativePath(path);
  if (!native.has_value()) {
    return std::unexpected(native.error());
  }
  auto file = OpenHandle(*native, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, OPEN_EXISTING,
                         FILE_ATTRIBUTE_NORMAL, operations_);
  if (!file.has_value()) {
    return std::unexpected(file.error());
  }
  const Status valid = CheckKind(file->get(), path, false, *operations_);
  if (!valid.has_value()) {
    return std::unexpected(valid.error());
  }
  return std::make_unique<WindowsSequentialFile>(std::move(*file), path, operations_);
}

Result<std::unique_ptr<RandomAccessFile>> WindowsFileSystem::OpenRandomAccess(
    const std::filesystem::path& path, std::optional<std::uint64_t> expected_size) {
  auto native = NativePath(path);
  if (!native.has_value()) {
    return std::unexpected(native.error());
  }
  auto file = OpenHandle(*native, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, OPEN_EXISTING,
                         FILE_FLAG_OVERLAPPED, operations_);
  if (!file.has_value()) {
    return std::unexpected(file.error());
  }
  const Status valid = CheckKind(file->get(), path, false, *operations_);
  if (!valid.has_value()) {
    return std::unexpected(valid.error());
  }
  const auto copied = [&]() -> Result<std::unique_ptr<RandomAccessFile>> {
    return std::make_unique<WindowsRandomAccessFile>(std::move(*file), path, operations_);
  };
  if (mmap_limiter_ == nullptr || !expected_size.has_value() || *expected_size == 0 ||
      *expected_size > std::numeric_limits<std::size_t>::max() ||
      *expected_size > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) ||
      !mmap_limiter_->Acquire()) {
    return copied();
  }
  PendingWindowsMapping pending(mmap_limiter_, operations_);
  LARGE_INTEGER size{};
  if (!operations_->Size(file->get(), &size)) {
    return std::unexpected(FileError("mapped file size", path, ::GetLastError()));
  }
  if (size.QuadPart < 0 || static_cast<std::uint64_t>(size.QuadPart) != *expected_size) {
    return copied();
  }
  ScopedHandle section(operations_->Mapping(file->get()), operations_);
  if (!section.valid()) {
    return std::unexpected(FileError("create file mapping", path, ::GetLastError()));
  }
  const void* view = operations_->Map(section.get());
  if (view == nullptr) {
    return std::unexpected(FileError("map file view", path, ::GetLastError()));
  }
  pending.SetView(view);
  const Status section_closed = CloseHandle(section, path);
  if (!section_closed.has_value()) {
    return std::unexpected(section_closed.error());
  }
  const Status file_closed = CloseHandle(*file, path);
  if (!file_closed.has_value()) {
    return std::unexpected(file_closed.error());
  }
  auto mapped = std::make_unique<WindowsMappedRandomAccessFile>(
      view, static_cast<std::size_t>(*expected_size), mmap_limiter_, operations_);
  pending.Commit();
  return mapped;
}

Result<std::unique_ptr<WritableFile>> WindowsFileSystem::OpenWritable(
    const std::filesystem::path& path) {
  return OpenWriter(path, false, operations_);
}

Result<std::unique_ptr<WritableFile>> WindowsFileSystem::OpenAppendable(
    const std::filesystem::path& path) {
  return OpenWriter(path, true, operations_);
}

Result<bool> WindowsFileSystem::FileExists(const std::filesystem::path& path) const {
  auto native = NativePath(path);
  if (!native.has_value()) {
    return std::unexpected(native.error());
  }
  if (::GetFileAttributesW(native->c_str()) != INVALID_FILE_ATTRIBUTES) {
    return true;
  }
  const DWORD error = ::GetLastError();
  if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND) {
    return false;
  }
  return std::unexpected(FileError("attributes", path, error));
}

Result<std::vector<std::filesystem::path>> WindowsFileSystem::ListDirectory(
    const std::filesystem::path& path) const {
  auto native = NativePath(path);
  if (!native.has_value()) {
    return std::unexpected(native.error());
  }
  auto directory = OpenDirectory(*native, operations_);
  if (!directory.has_value()) {
    return std::unexpected(directory.error());
  }
  const Status closed = CloseHandle(*directory, path);
  if (!closed.has_value()) {
    return std::unexpected(closed.error());
  }
  WIN32_FIND_DATAW information{};
  const HANDLE found = operations_->Find((*native / L"*").c_str(), &information);
  if (found == INVALID_HANDLE_VALUE) {
    const DWORD error = ::GetLastError();
    if (error == ERROR_FILE_NOT_FOUND) {
      return std::vector<std::filesystem::path>{};
    }
    return std::unexpected(FileError("list directory", path, error));
  }
  struct FindCloser {
    const WindowsFileOperations* operations;
    void operator()(void* handle) const noexcept {
      static_cast<void>(operations->CloseFind(handle));
    }
  };
  std::unique_ptr<void, FindCloser> guard(found, FindCloser{operations_.get()});
  std::vector<std::filesystem::path> result;
  do {
    const std::wstring_view name(information.cFileName);
    if (name != L"." && name != L"..") {
      result.emplace_back(name);
    }
  } while (operations_->Next(found, &information));
  const DWORD error = ::GetLastError();
  if (error != ERROR_NO_MORE_FILES) {
    return std::unexpected(FileError("list directory", path, error));
  }
  if (!operations_->CloseFind(guard.release())) {
    return std::unexpected(FileError("close directory enumeration", path, ::GetLastError()));
  }
  return result;
}

Result<std::uint64_t> WindowsFileSystem::FileSize(const std::filesystem::path& path) const {
  auto native = NativePath(path);
  if (!native.has_value()) {
    return std::unexpected(native.error());
  }
  auto file = OpenHandle(*native, FILE_READ_ATTRIBUTES, MetadataSharing, OPEN_EXISTING,
                         FILE_ATTRIBUTE_NORMAL, operations_);
  if (!file.has_value()) {
    return std::unexpected(file.error());
  }
  const Status valid = CheckKind(file->get(), path, false, *operations_);
  if (!valid.has_value()) {
    return std::unexpected(valid.error());
  }
  LARGE_INTEGER size{};
  if (!::GetFileSizeEx(file->get(), &size)) {
    return std::unexpected(FileError("file size", path, ::GetLastError()));
  }
  const Status closed = CloseHandle(*file, path);
  if (!closed.has_value()) {
    return std::unexpected(closed.error());
  }
  return static_cast<std::uint64_t>(size.QuadPart);
}

Status WindowsFileSystem::CreateDirectory(const std::filesystem::path& path) {
  auto native = NativePath(path);
  if (!native.has_value()) {
    return std::unexpected(native.error());
  }
  if (::CreateDirectoryW(native->c_str(), nullptr)) {
    return {};
  }
  const DWORD error = ::GetLastError();
  if (error == ERROR_ALREADY_EXISTS) {
    const DWORD attributes = ::GetFileAttributesW(native->c_str());
    if (attributes == INVALID_FILE_ATTRIBUTES) {
      return std::unexpected(FileError("directory attributes", path, ::GetLastError()));
    }
    if ((attributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
      return {};
    }
    return std::unexpected(Error::InvalidArgument("an existing file is not a directory"));
  }
  return std::unexpected(FileError("create directory", path, error));
}

Status WindowsFileSystem::RemoveFile(const std::filesystem::path& path) {
  auto native = NativePath(path);
  if (!native.has_value()) {
    return std::unexpected(native.error());
  }
  if (!::DeleteFileW(native->c_str())) {
    return std::unexpected(FileError("remove file", path, ::GetLastError()));
  }
  return {};
}

Status WindowsFileSystem::RemoveDirectory(const std::filesystem::path& path) {
  auto native = NativePath(path);
  if (!native.has_value()) {
    return std::unexpected(native.error());
  }
  if (!::RemoveDirectoryW(native->c_str())) {
    return std::unexpected(FileError("remove directory", path, ::GetLastError()));
  }
  return {};
}

Status WindowsFileSystem::RenameFile(const std::filesystem::path& source,
                                     const std::filesystem::path& destination) {
  auto from = NativePath(source);
  auto to = NativePath(destination);
  if (!from.has_value()) {
    return std::unexpected(from.error());
  }
  if (!to.has_value()) {
    return std::unexpected(to.error());
  }
  if (!::MoveFileExW(from->c_str(), to->c_str(), MOVEFILE_REPLACE_EXISTING)) {
    const DWORD error = ::GetLastError();
    return std::unexpected(FileError(
        "rename to " + PathUtf8(destination).value_or("<unrepresentable>"), source, error));
  }
  return {};
}

Status WindowsFileSystem::SyncDirectory(const std::filesystem::path& path) {
  auto native = NativePath(path);
  if (!native.has_value()) {
    return std::unexpected(native.error());
  }
  if (!allow_weak_namespace_durability_) {
    return std::unexpected(Error::NotSupported("Windows has no strict directory sync barrier"));
  }
  auto directory = OpenDirectory(*native, operations_);
  if (!directory.has_value()) {
    return std::unexpected(directory.error());
  }
  return CloseHandle(*directory, path);
}

Result<std::unique_ptr<FileLock>> WindowsFileSystem::LockFile(const std::filesystem::path& path) {
  auto native = NativePath(path);
  if (!native.has_value()) {
    return std::unexpected(native.error());
  }
  auto file = OpenHandle(*native, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                         OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, operations_, true);
  if (!file.has_value()) {
    return std::unexpected(file.error());
  }
  const Status valid = CheckKind(file->get(), path, false, *operations_);
  if (!valid.has_value()) {
    return std::unexpected(valid.error());
  }
  OVERLAPPED overlapped{};
  if (!::LockFileEx(file->get(), LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, MAXDWORD,
                    MAXDWORD, &overlapped)) {
    const DWORD error = ::GetLastError();
    if (error == ERROR_LOCK_VIOLATION || error == ERROR_SHARING_VIOLATION) {
      return std::unexpected(Error::Busy(std::string(FileError("lock", path, error).message())));
    }
    return std::unexpected(FileError("lock", path, error));
  }
  return std::make_unique<WindowsFileLock>(std::move(*file));
}

}  // namespace modern_leveldb
