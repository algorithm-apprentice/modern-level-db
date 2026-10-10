#ifndef MODERN_LEVELDB_PLATFORM_FILE_SYSTEM_H_
#define MODERN_LEVELDB_PLATFORM_FILE_SYSTEM_H_

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <vector>

#include "modern_leveldb/base/bytes.h"
#include "modern_leveldb/base/result.h"

namespace modern_leveldb {

// Stateful stream handle: reads and destruction require external synchronization.
class SequentialFile {
public:
    SequentialFile() = default;
    SequentialFile(const SequentialFile&) = delete;
    SequentialFile& operator=(const SequentialFile&) = delete;
    SequentialFile(SequentialFile&&) = delete;
    SequentialFile& operator=(SequentialFile&&) = delete;
    virtual ~SequentialFile() = default;

    // Fills at most output.size() bytes and advances the file position. Short reads
    // are allowed; zero with a nonempty output means EOF. Errors are not EOF.
    [[nodiscard]] virtual Result<std::size_t> Read(MutableByteView output) = 0;
};

// Positioned reads can run concurrently with distinct output buffers. The owner
// keeps the handle/backing file alive and immutable until all readers finish.
class RandomAccessFile {
public:
    RandomAccessFile() = default;
    RandomAccessFile(const RandomAccessFile&) = delete;
    RandomAccessFile& operator=(const RandomAccessFile&) = delete;
    RandomAccessFile(RandomAccessFile&&) = delete;
    RandomAccessFile& operator=(RandomAccessFile&&) = delete;
    virtual ~RandomAccessFile() = default;

    // Reads at an absolute offset without changing shared stream position; the
    // byte count is bounded by output.size(). A caller needing exact bytes loops.
    [[nodiscard]] virtual Result<std::size_t> Read(std::uint64_t offset,
                                                   MutableByteView output) const = 0;
    // Returns an exact immutable view that remains valid until this file is
    // destroyed, or nothing when stable borrowed reads are unavailable.
    [[nodiscard]] virtual std::optional<ByteView> TryReadView(std::uint64_t,
                                                              std::size_t) const noexcept {
        return std::nullopt;
    }
};

// Append-only handle with externally serialized operations. Visibility to the OS,
// file-content durability, and namespace durability are distinct boundaries.
// See docs/learning/04-wal-and-recovery.md.
class WritableFile {
public:
    WritableFile() = default;
    WritableFile(const WritableFile&) = delete;
    WritableFile& operator=(const WritableFile&) = delete;
    WritableFile(WritableFile&&) = delete;
    WritableFile& operator=(WritableFile&&) = delete;
    virtual ~WritableFile() = default;

    // Borrows data for the call. A failed append may have written some bytes;
    // callers must not assume rollback or safely retry a logical record.
    [[nodiscard]] virtual Status Append(ByteView data) = 0;
    // Pushes application-buffered bytes toward the OS; does not promise durability.
    [[nodiscard]] virtual Status Flush() = 0;
    // Flushes pending bytes and requests durable file contents, not directory names.
    [[nodiscard]] virtual Status Sync() = 0;
    // Releases the file resource. Closing is not a substitute for a prior Sync.
    [[nodiscard]] virtual Status Close() = 0;
};

// RAII exclusive directory-ownership token; destruction releases the lock.
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

// Narrow I/O boundary used by the engine and fault-injection backends. Native
// filesystem operations support concurrent calls; stateful open handles have
// their own contracts above. Returned resources own their handles, not this object.
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
    // expected_size permits a backend's exact-size immutable mapping policy;
    // unavailable mappings fall back to copied reads rather than changing callers.
    [[nodiscard]] virtual Result<std::unique_ptr<RandomAccessFile>> OpenRandomAccess(
        const std::filesystem::path& path,
        std::optional<std::uint64_t> expected_size = std::nullopt) = 0;
    // Creates or truncates a file. OpenAppendable creates if absent and appends
    // to existing contents; callers supply the existing size to WAL framing.
    [[nodiscard]] virtual Result<std::unique_ptr<WritableFile>> OpenWritable(
        const std::filesystem::path& path) = 0;
    [[nodiscard]] virtual Result<std::unique_ptr<WritableFile>> OpenAppendable(
        const std::filesystem::path& path) = 0;

    // Absence is false, while permission/I/O failures remain explicit errors.
    [[nodiscard]] virtual Result<bool> FileExists(const std::filesystem::path& path) const = 0;
    [[nodiscard]] virtual Result<std::vector<std::filesystem::path>> ListDirectory(
        const std::filesystem::path& path) const = 0;
    [[nodiscard]] virtual Result<std::uint64_t> FileSize(
        const std::filesystem::path& path) const = 0;

    [[nodiscard]] virtual Status CreateDirectory(const std::filesystem::path& path) = 0;
    [[nodiscard]] virtual Status RemoveFile(const std::filesystem::path& path) = 0;
    [[nodiscard]] virtual Status RemoveDirectory(const std::filesystem::path& path) = 0;
    [[nodiscard]] virtual Status RenameFile(const std::filesystem::path& source,
                                            const std::filesystem::path& destination) = 0;
    // Requests persistence of namespace changes where supported. Native Windows
    // accepts only explicit weak-namespace mode: success is not a durable-name guarantee.
    [[nodiscard]] virtual Status SyncDirectory(const std::filesystem::path& path) = 0;
    // Non-blocking exclusive ownership; conflicts report Busy, not a wait for release.
    [[nodiscard]] virtual Result<std::unique_ptr<FileLock>> LockFile(
        const std::filesystem::path& path) = 0;
};

// Creates the admitted native backend and prepares directory without creating
// it. Windows requires explicit weak namespace-durability consent; POSIX leaves
// directory unchanged. On failure, directory is unchanged.
[[nodiscard]] Result<std::unique_ptr<FileSystem>> CreateDefaultFileSystem(
    std::filesystem::path& directory, bool allow_weak_namespace_durability, bool allow_mmap_reads);

}  // namespace modern_leveldb

#endif  // MODERN_LEVELDB_PLATFORM_FILE_SYSTEM_H_
