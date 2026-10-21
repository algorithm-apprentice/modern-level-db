#ifndef MODERN_LEVELDB_TESTS_SUPPORT_CRC32C_REFERENCE_H_
#define MODERN_LEVELDB_TESTS_SUPPORT_CRC32C_REFERENCE_H_

#include <cstddef>
#include <cstdint>

#include "modern_leveldb/base/bytes.h"

namespace modern_leveldb::test_support {

inline std::uint32_t BitwiseExtend(std::uint32_t initial, ByteView input) {
    std::uint32_t crc = ~initial;
    for (const std::byte byte : input) {
        crc ^= std::to_integer<std::uint32_t>(byte);
        for (int bit = 0; bit < 8; ++bit) {
            crc = (crc & 1U) != 0U ? (crc >> 1U) ^ 0x82f63b78U : crc >> 1U;
        }
    }
    return ~crc;
}

}  // namespace modern_leveldb::test_support

#endif  // MODERN_LEVELDB_TESTS_SUPPORT_CRC32C_REFERENCE_H_
