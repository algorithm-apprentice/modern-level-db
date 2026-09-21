#ifndef MODERN_LEVELDB_BASE_BYTES_H_
#define MODERN_LEVELDB_BASE_BYTES_H_

#include <cstddef>
#include <span>
#include <string_view>

namespace modern_leveldb {

using ByteView = std::span<const std::byte>;
using MutableByteView = std::span<std::byte>;

// Returns a non-owning byte view over the string storage.
[[nodiscard]] inline ByteView AsBytes(std::string_view value) noexcept {
  return std::as_bytes(std::span<const char>(value.data(), value.size()));
}

// Returns a non-owning mutable byte view over character storage.
[[nodiscard]] inline MutableByteView AsWritableBytes(std::span<char> value) noexcept {
  return std::as_writable_bytes(value);
}

// Returns a non-owning string view over byte storage.
[[nodiscard]] inline std::string_view AsStringView(ByteView value) noexcept {
  return {reinterpret_cast<const char*>(value.data()), value.size()};
}

}  // namespace modern_leveldb

#endif  // MODERN_LEVELDB_BASE_BYTES_H_
