#ifndef MODERN_LEVELDB_BASE_CODING_INTERNAL_H_
#define MODERN_LEVELDB_BASE_CODING_INTERNAL_H_

#include <cstdint>

#include "modern_leveldb/base/bytes.h"

namespace modern_leveldb {

// Requires enough output for the canonical encoding and advances past it.
void EncodeVarint32Trusted(MutableByteView& output, std::uint32_t value) noexcept;

}  // namespace modern_leveldb

#endif  // MODERN_LEVELDB_BASE_CODING_INTERNAL_H_
