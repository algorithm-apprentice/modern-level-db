#ifndef MODERN_LEVELDB_PLATFORM_PATH_H_
#define MODERN_LEVELDB_PLATFORM_PATH_H_

#include <filesystem>
#include <string>

#include "modern_leveldb/base/result.h"

namespace modern_leveldb {

[[nodiscard]] Result<std::string> PathUtf8(const std::filesystem::path& path);

}  // namespace modern_leveldb

#endif  // MODERN_LEVELDB_PLATFORM_PATH_H_
