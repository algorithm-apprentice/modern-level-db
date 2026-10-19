// Preserve the SDK-first consumer include order.
// clang-format off
#include <windows.h>
#include "modern_leveldb/file_system.h"
// clang-format on

#include <type_traits>

#if !defined(CreateDirectory) || !defined(RemoveDirectory)
#error "The public filesystem header must preserve Windows SDK macros."
#endif

namespace {

using DirectoryOperation =
    modern_leveldb::Status (modern_leveldb::FileSystem::*)(const std::filesystem::path&);

static_assert(std::is_same_v<decltype(&modern_leveldb::FileSystem::CreateDir), DirectoryOperation>);
static_assert(std::is_same_v<decltype(&modern_leveldb::FileSystem::RemoveDir), DirectoryOperation>);

}  // namespace
