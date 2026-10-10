#include "platform/path.h"

#include <string>

namespace modern_leveldb {

Result<std::string> PathUtf8(const std::filesystem::path& path) {
    try {
        const std::u8string utf8 = path.generic_u8string();
        return std::string(reinterpret_cast<const char*>(utf8.data()), utf8.size());
        // GCOVR_EXCL_START: no portable deterministic path triggers conversion failure
    } catch (const std::filesystem::filesystem_error&) {
        return std::unexpected(Error::InvalidArgument("path is not representable as UTF-8"));
    }
    // GCOVR_EXCL_STOP
}

}  // namespace modern_leveldb
