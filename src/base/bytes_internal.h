#ifndef MODERN_LEVELDB_BASE_BYTES_INTERNAL_H_
#define MODERN_LEVELDB_BASE_BYTES_INTERNAL_H_

#include <algorithm>
#include <cstddef>
#include <vector>

#include "modern_leveldb/base/bytes.h"

namespace modern_leveldb {

inline std::size_t SharedPrefixLength(ByteView left, ByteView right) noexcept {
    const std::size_t limit = std::min(left.size(), right.size());
    std::size_t shared = 0;
    while (shared < limit && left[shared] == right[shared]) {
        ++shared;
    }
    return shared;
}

// The source borrows a separate owner that remains live throughout the copy.
inline void CopyLookupValue(ByteView source, std::vector<std::byte>& value) {
    value.resize(source.size());
    std::ranges::copy(source, value.begin());
}

}  // namespace modern_leveldb

#endif  // MODERN_LEVELDB_BASE_BYTES_INTERNAL_H_
