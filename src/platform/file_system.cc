#include "platform/file_system.h"

#include <memory>
#include <utility>

#if defined(MODERN_LEVELDB_HAVE_POSIX_FILE_SYSTEM)
#include "platform/posix_file_system.h"
#elif defined(MODERN_LEVELDB_HAVE_WINDOWS_FILE_SYSTEM)
#include "platform/windows_file_system.h"
#endif

namespace modern_leveldb {

Result<std::unique_ptr<FileSystem>> CreateDefaultFileSystem(std::filesystem::path& directory,
                                                            bool allow_weak_namespace_durability,
                                                            bool allow_mmap_reads) {
#if defined(MODERN_LEVELDB_HAVE_POSIX_FILE_SYSTEM)
    static_cast<void>(directory);
    static_cast<void>(allow_weak_namespace_durability);
    return std::make_unique<PosixFileSystem>(allow_mmap_reads);
#elif defined(MODERN_LEVELDB_HAVE_WINDOWS_FILE_SYSTEM)
    auto file_system =
        std::make_unique<WindowsFileSystem>(allow_weak_namespace_durability, allow_mmap_reads);
    auto prepared = file_system->PrepareDatabaseDirectory(directory);
    if (!prepared.has_value()) {
        return std::unexpected(prepared.error());
    }
    directory = std::move(*prepared);
    return std::unique_ptr<FileSystem>(std::move(file_system));
#else
    static_cast<void>(directory);
    static_cast<void>(allow_weak_namespace_durability);
    static_cast<void>(allow_mmap_reads);
    return std::unexpected(Error::NotSupported("this platform has no default file system"));
#endif
}

}  // namespace modern_leveldb
