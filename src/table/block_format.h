#ifndef MODERN_LEVELDB_TABLE_BLOCK_FORMAT_H_
#define MODERN_LEVELDB_TABLE_BLOCK_FORMAT_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>
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
    [[nodiscard]] static BlockContents Borrowed(ByteView contents) noexcept;

    BlockContents(const BlockContents&) = delete;
    BlockContents& operator=(const BlockContents&) = delete;
    BlockContents(BlockContents&&) noexcept = default;
    BlockContents& operator=(BlockContents&&) = delete;
    ~BlockContents() = default;

    [[nodiscard]] ByteView data() const noexcept;
    [[nodiscard]] bool cacheable() const noexcept { return !borrowed_.has_value(); }

private:
    explicit BlockContents(std::vector<std::byte> contents) noexcept
        : owned_(std::move(contents)) {}
    explicit BlockContents(ByteView contents) noexcept : borrowed_(contents) {}

    std::vector<std::byte> owned_;
    std::optional<ByteView> borrowed_;
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
