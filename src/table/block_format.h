#ifndef MODERN_LEVELDB_TABLE_BLOCK_FORMAT_H_
#define MODERN_LEVELDB_TABLE_BLOCK_FORMAT_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <utility>
#include <variant>
#include <vector>

#include "modern_leveldb/base/bytes.h"
#include "modern_leveldb/base/result.h"
#include "table/compression.h"

namespace modern_leveldb {

inline constexpr std::size_t BlockHandleMaxEncodedSize = 20;
inline constexpr std::size_t BlockTrailerSize = 5;
inline constexpr std::size_t FooterSize = 48;
inline constexpr std::uint64_t TableMagicNumber = 0xdb4775248b80fb57ULL;

// Locates a stored block in a table file. The size excludes the block trailer.
struct BlockHandle {
    std::uint64_t offset;
    std::uint64_t size;
};

void AppendBlockHandle(std::vector<std::byte>& output, BlockHandle handle);
// Consumes one handle from the front of input, which is unchanged on failure.
[[nodiscard]] Result<BlockHandle> ConsumeBlockHandle(ByteView& input);

struct Footer {
    BlockHandle metaindex;
    BlockHandle index;
};

// Decoded block bytes are either owned and independently cacheable or borrowed
// from an immutable random-access file that must outlive them.
class BlockContents final {
public:
    [[nodiscard]] static BlockContents Owned(std::vector<std::byte> contents) noexcept;
    [[nodiscard]] static BlockContents Owned(DecodedBlockBytes contents) noexcept;
    [[nodiscard]] static BlockContents Borrowed(ByteView contents) noexcept;

    BlockContents(const BlockContents&) = delete;
    BlockContents& operator=(const BlockContents&) = delete;
    BlockContents(BlockContents&&) noexcept = default;
    BlockContents& operator=(BlockContents&&) = delete;
    ~BlockContents() = default;

    [[nodiscard]] ByteView data() const noexcept;
    [[nodiscard]] bool cacheable() const noexcept {
        return !std::holds_alternative<ByteView>(storage_);
    }

private:
    // Alternatives are only constructed or moved without throwing, never
    // reassigned, so data() cannot visit a valueless variant.
    using Storage = std::variant<ByteView, std::vector<std::byte>, DecodedBlockBytes>;

    explicit BlockContents(std::vector<std::byte> contents) noexcept
        : storage_(std::in_place_type<std::vector<std::byte>>, std::move(contents)) {}
    explicit BlockContents(DecodedBlockBytes contents) noexcept
        : storage_(std::in_place_type<DecodedBlockBytes>, std::move(contents)) {}
    explicit BlockContents(ByteView contents) noexcept
        : storage_(std::in_place_type<ByteView>, contents) {}

    Storage storage_;
};

[[nodiscard]] std::array<std::byte, FooterSize> EncodeFooter(const Footer& footer);
[[nodiscard]] Result<Footer> DecodeFooter(std::span<const std::byte, FooterSize> encoded);

// Returns the trailer for the bytes and compression type written to the file.
[[nodiscard]] std::array<std::byte, BlockTrailerSize> EncodeBlockTrailer(
    ByteView contents, BlockCompression type) noexcept;
// Verifies a stored block and returns decoded contents. Uncompressed contents
// reuse the input buffer.
[[nodiscard]] Result<BlockContents> DecodeStoredBlock(std::vector<std::byte> stored);
// Verifies borrowed stored bytes. Uncompressed contents remain borrowed;
// compressed contents are returned in owned decompressed storage.
[[nodiscard]] Result<BlockContents> DecodeStoredBlock(ByteView stored);

}  // namespace modern_leveldb

#endif  // MODERN_LEVELDB_TABLE_BLOCK_FORMAT_H_
