#ifndef MODERN_LEVELDB_PLATFORM_WINDOWS_FILE_SYSTEM_INTERNAL_H_
#define MODERN_LEVELDB_PLATFORM_WINDOWS_FILE_SYSTEM_INTERNAL_H_

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <winioctl.h>

// FileSystem uses these names; native calls always use explicit W spellings.
#undef CreateDirectory
#undef RemoveDirectory

#include <algorithm>
#include <cstddef>
#include <limits>

namespace modern_leveldb {

[[nodiscard]] inline DWORD WindowsIoRequestSize(std::size_t size) noexcept {
  return static_cast<DWORD>(
      std::min(size, static_cast<std::size_t>(std::numeric_limits<DWORD>::max())));
}

// Private fault seam; returned file objects retain it for their whole lifetime.
class WindowsFileOperations {
 public:
  virtual ~WindowsFileOperations() = default;
  [[nodiscard]] virtual HANDLE Open(const wchar_t* path, DWORD access, DWORD share,
                                    DWORD disposition, DWORD flags) const noexcept {
    return ::CreateFileW(path, access, share, nullptr, disposition, flags, nullptr);
  }
  [[nodiscard]] virtual BOOL Inspect(HANDLE file,
                                     BY_HANDLE_FILE_INFORMATION* information) const noexcept {
    return ::GetFileInformationByHandle(file, information);
  }
  [[nodiscard]] virtual HANDLE Event() const noexcept {
    return ::CreateEventW(nullptr, TRUE, FALSE, nullptr);
  }
  [[nodiscard]] virtual BOOL Read(HANDLE file, void* output, DWORD size, DWORD* read,
                                  OVERLAPPED* overlapped) const noexcept {
    return ::ReadFile(file, output, size, read, overlapped);
  }
  [[nodiscard]] virtual BOOL Complete(HANDLE file, OVERLAPPED* overlapped,
                                      DWORD* read) const noexcept {
    return ::GetOverlappedResult(file, overlapped, read, TRUE);
  }
  [[nodiscard]] virtual BOOL Write(HANDLE file, const void* data, DWORD size,
                                   DWORD* written) const noexcept {
    return ::WriteFile(file, data, size, written, nullptr);
  }
  [[nodiscard]] virtual BOOL Sync(HANDLE file) const noexcept { return ::FlushFileBuffers(file); }
  [[nodiscard]] virtual BOOL Close(HANDLE handle) const noexcept { return ::CloseHandle(handle); }
  [[nodiscard]] virtual HANDLE Find(const wchar_t* pattern,
                                    WIN32_FIND_DATAW* information) const noexcept {
    return ::FindFirstFileW(pattern, information);
  }
  [[nodiscard]] virtual BOOL Next(HANDLE handle, WIN32_FIND_DATAW* information) const noexcept {
    return ::FindNextFileW(handle, information);
  }
  [[nodiscard]] virtual BOOL CloseFind(HANDLE handle) const noexcept { return ::FindClose(handle); }
  [[nodiscard]] virtual UINT DriveType(const wchar_t* volume) const noexcept {
    return ::GetDriveTypeW(volume);
  }
  [[nodiscard]] virtual BOOL VolumeInformation(const wchar_t* volume, wchar_t* file_system,
                                               DWORD size) const noexcept {
    return ::GetVolumeInformationW(volume, nullptr, 0, nullptr, nullptr, nullptr, file_system,
                                   size);
  }
};

}  // namespace modern_leveldb

#endif  // MODERN_LEVELDB_PLATFORM_WINDOWS_FILE_SYSTEM_INTERNAL_H_
