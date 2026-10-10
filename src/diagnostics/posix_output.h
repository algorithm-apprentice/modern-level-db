#ifndef MODERN_LEVELDB_DIAGNOSTICS_POSIX_OUTPUT_H_
#define MODERN_LEVELDB_DIAGNOSTICS_POSIX_OUTPUT_H_

#include "modern_leveldb/base/bytes.h"
#include "modern_leveldb/base/result.h"
#include "platform/file_system.h"

namespace modern_leveldb {

[[nodiscard]] Status IgnoreBrokenPipeSignal();

// Writes synchronously to a borrowed POSIX descriptor without closing it.
class PosixOutputFile final : public WritableFile {
public:
    explicit PosixOutputFile(int descriptor) noexcept : descriptor_(descriptor) {}

    [[nodiscard]] Status Append(ByteView data) override;
    [[nodiscard]] Status Flush() override;
    [[nodiscard]] Status Sync() override;
    [[nodiscard]] Status Close() override;

private:
    [[nodiscard]] Status CheckOpen() const;

    int descriptor_;
    bool closed_ = false;
};

}  // namespace modern_leveldb

#endif  // MODERN_LEVELDB_DIAGNOSTICS_POSIX_OUTPUT_H_
