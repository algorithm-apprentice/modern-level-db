#ifndef MODERN_LEVELDB_DIAGNOSTICS_DUMP_COMMAND_H_
#define MODERN_LEVELDB_DIAGNOSTICS_DUMP_COMMAND_H_

#include <span>
#include <string_view>

#include "platform/file_system.h"

namespace modern_leveldb {

// Runs command arguments that exclude the executable name.
[[nodiscard]] int RunDiagnosticTool(std::span<const std::string_view> arguments,
                                    FileSystem& file_system, WritableFile& output,
                                    WritableFile& errors);

}  // namespace modern_leveldb

#endif  // MODERN_LEVELDB_DIAGNOSTICS_DUMP_COMMAND_H_
