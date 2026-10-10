#ifndef MODERN_LEVELDB_DIAGNOSTICS_DUMP_FILE_H_
#define MODERN_LEVELDB_DIAGNOSTICS_DUMP_FILE_H_

#include <filesystem>
#include <string>

#include "modern_leveldb/base/bytes.h"
#include "modern_leveldb/base/result.h"
#include "platform/file_system.h"

namespace modern_leveldb {

[[nodiscard]] std::string EscapeDiagnosticBytes(ByteView bytes);
[[nodiscard]] Result<std::string> EscapeDiagnosticPath(const std::filesystem::path& path);
[[nodiscard]] Status AppendDiagnosticLine(WritableFile& output, std::string line);

// Inspects one canonical WAL, MANIFEST, or SSTable from a closed database or
// stable offline copy. The output must not alias any database file. Decoding
// visited records is not backup, repair, or certification of all file bytes.
// See docs/reference/storage-diagnostics.md.
[[nodiscard]] Status DumpFile(FileSystem& file_system, const std::filesystem::path& path,
                              WritableFile& output);

}  // namespace modern_leveldb

#endif  // MODERN_LEVELDB_DIAGNOSTICS_DUMP_FILE_H_
