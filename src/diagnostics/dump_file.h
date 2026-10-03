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

// Dumps one canonical WAL, MANIFEST, or SSTable file. The output must not
// alias the input or another database file.
[[nodiscard]] Status DumpFile(FileSystem& file_system, const std::filesystem::path& path,
                              WritableFile& output);

}  // namespace modern_leveldb

#endif  // MODERN_LEVELDB_DIAGNOSTICS_DUMP_FILE_H_
