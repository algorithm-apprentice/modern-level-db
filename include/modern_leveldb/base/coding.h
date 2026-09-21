#ifndef MODERN_LEVELDB_BASE_CODING_H_
#define MODERN_LEVELDB_BASE_CODING_H_

#include <cstddef>
#include <cstdint>
#include <vector>

#include "modern_leveldb/base/bytes.h"
#include "modern_leveldb/base/result.h"

namespace modern_leveldb {

void AppendFixed32(std::vector<std::byte>& output, std::uint32_t value);
void AppendFixed64(std::vector<std::byte>& output, std::uint64_t value);

[[nodiscard]] Result<std::uint32_t> ConsumeFixed32(ByteView& input);
[[nodiscard]] Result<std::uint64_t> ConsumeFixed64(ByteView& input);

void AppendVarint32(std::vector<std::byte>& output, std::uint32_t value);
void AppendVarint64(std::vector<std::byte>& output, std::uint64_t value);

[[nodiscard]] Result<std::uint32_t> ConsumeVarint32(ByteView& input);
[[nodiscard]] Result<std::uint64_t> ConsumeVarint64(ByteView& input);

// Appends a varint32 length and the bytes. The value may alias output.
// An oversized value returns InvalidArgument without changing output.
[[nodiscard]] Status AppendLengthPrefixed(std::vector<std::byte>& output, ByteView value);
[[nodiscard]] Result<ByteView> ConsumeLengthPrefixed(ByteView& input);

[[nodiscard]] std::size_t VarintLength(std::uint64_t value) noexcept;

}  // namespace modern_leveldb

#endif  // MODERN_LEVELDB_BASE_CODING_H_
