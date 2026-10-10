#include "diagnostics/windows_output.h"

#include <cassert>
#include <string>
#include <utility>

namespace modern_leveldb {

WindowsOutputFile::WindowsOutputFile(HANDLE handle)
    : WindowsOutputFile(handle, std::make_shared<WindowsFileOperations>()) {}
WindowsOutputFile::WindowsOutputFile(HANDLE handle,
                                     std::shared_ptr<WindowsFileOperations> operations)
    : WindowsOutputFile(handle, std::nullopt, std::move(operations)) {}
WindowsOutputFile::WindowsOutputFile(HANDLE handle, std::optional<DWORD> acquisition_error,
                                     std::shared_ptr<WindowsFileOperations> operations)
    : handle_(handle), acquisition_error_(acquisition_error), operations_(std::move(operations)) {
    assert(operations_ != nullptr);
}
WindowsOutputFile WindowsOutputFile::Standard(DWORD stream) {
    const HANDLE handle = ::GetStdHandle(stream);
    const std::optional<DWORD> error =
        handle == INVALID_HANDLE_VALUE ? std::optional<DWORD>(::GetLastError()) : std::nullopt;
    return WindowsOutputFile(handle, error, std::make_shared<WindowsFileOperations>());
}
Status WindowsOutputFile::CheckOpen() const {
    if (closed_) {
        return std::unexpected(Error::InvalidArgument("output used after close"));
    }
    if (acquisition_error_.has_value()) {
        return std::unexpected(Error::Io("get standard output handle (Win32 " +
                                         std::to_string(*acquisition_error_) + ")"));
    }
    if (handle_ == nullptr) {
        return std::unexpected(Error::Io("output handle is absent"));
    }
    return {};
}
Status WindowsOutputFile::Append(ByteView bytes) {
    const Status open = CheckOpen();
    if (!open.has_value()) {
        return open;
    }
    while (!bytes.empty()) {
        DWORD written = 0;
        if (!operations_->Write(handle_, bytes.data(), WindowsIoRequestSize(bytes.size()),
                                &written)) {
            const DWORD error = ::GetLastError();
            return std::unexpected(Error::Io("write output (Win32 " + std::to_string(error) + ")"));
        }
        if (written == 0) {
            return std::unexpected(Error::Io("write output made no progress"));
        }
        bytes = bytes.subspan(written);
    }
    return {};
}
Status WindowsOutputFile::Flush() { return CheckOpen(); }
Status WindowsOutputFile::Sync() { return CheckOpen(); }
Status WindowsOutputFile::Close() {
    const Status open = CheckOpen();
    if (!open.has_value()) {
        return open;
    }
    closed_ = true;
    return {};
}

}  // namespace modern_leveldb
