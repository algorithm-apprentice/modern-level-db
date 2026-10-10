#ifndef MODERN_LEVELDB_DIAGNOSTICS_WINDOWS_OUTPUT_H_
#define MODERN_LEVELDB_DIAGNOSTICS_WINDOWS_OUTPUT_H_

#include <memory>
#include <optional>

#include "platform/file_system.h"
#include "platform/windows_file_system_internal.h"

namespace modern_leveldb {

// Borrows a synchronous handle; the caller retains ownership and lifetime.
class WindowsOutputFile final : public WritableFile {
public:
    explicit WindowsOutputFile(HANDLE handle);
    WindowsOutputFile(HANDLE handle, std::shared_ptr<WindowsFileOperations> operations);
    [[nodiscard]] static WindowsOutputFile Standard(DWORD stream);
    [[nodiscard]] Status Append(ByteView data) override;
    [[nodiscard]] Status Flush() override;
    [[nodiscard]] Status Sync() override;
    [[nodiscard]] Status Close() override;

private:
    WindowsOutputFile(HANDLE handle, std::optional<DWORD> acquisition_error,
                      std::shared_ptr<WindowsFileOperations> operations);
    [[nodiscard]] Status CheckOpen() const;
    HANDLE handle_;
    std::optional<DWORD> acquisition_error_;
    std::shared_ptr<WindowsFileOperations> operations_;
    bool closed_ = false;
};

}  // namespace modern_leveldb

#endif  // MODERN_LEVELDB_DIAGNOSTICS_WINDOWS_OUTPUT_H_
