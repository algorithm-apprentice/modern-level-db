#include "table/block_format.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>
#include <span>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "base/coding_internal.h"
#include "modern_leveldb/base/bytes.h"
#include "modern_leveldb/base/coding.h"
#include "modern_leveldb/base/crc32c.h"
#include "modern_leveldb/base/result.h"

namespace modern_leveldb {
namespace {

constexpr std::size_t FooterHandlesSize = 2 * BlockHandleMaxEncodedSize;

struct StoredBlock {
    ByteView contents;
    BlockCompression compression;
};

std::uint32_t BlockChecksum(ByteView contents, std::byte type) noexcept {
    return MaskCrc32c(ExtendCrc32c(Crc32c(contents), ByteView(&type, 1)));
}

bool StoredBlockChecksumMatches(ByteView stored, std::size_t contents_size) noexcept {
    const std::uint32_t checksum =
        DecodeFixedTrusted<std::uint32_t>(stored.last<sizeof(std::uint32_t)>());
    return checksum == MaskCrc32c(Crc32c(stored.first(contents_size + 1)));
}

std::optional<BlockCompression> DecodeCompressionType(std::byte encoded) noexcept {
    const auto compression = static_cast<BlockCompression>(encoded);
    switch (compression) {
        case BlockCompression::None:
        case BlockCompression::Snappy:
        case BlockCompression::Zstd:
            return compression;
    }
    return std::nullopt;
}

Result<StoredBlock> ValidateStoredBlock(ByteView stored) {
    if (stored.size() < BlockTrailerSize) {
        return std::unexpected(Error::Corruption("stored block is shorter than its trailer"));
    }
    const std::size_t contents_size = stored.size() - BlockTrailerSize;
    const std::byte type = stored[contents_size];
    if (!StoredBlockChecksumMatches(stored, contents_size)) {
        return std::unexpected(Error::Corruption("block checksum mismatch"));
    }
    const std::optional<BlockCompression> compression = DecodeCompressionType(type);
    if (!compression.has_value()) {
        return std::unexpected(Error::Corruption("block has an unknown compression type"));
    }
    return StoredBlock{
        .contents = stored.first(contents_size),
        .compression = *compression,
    };
}

Result<Footer> DecodeFooterHandles(ByteView& handles) {
    const Result<BlockHandle> metaindex = ConsumeBlockHandle(handles);
    if (!metaindex.has_value()) {
        return std::unexpected(metaindex.error());
    }
    const Result<BlockHandle> index = ConsumeBlockHandle(handles);
    if (!index.has_value()) {
        return std::unexpected(index.error());
    }
    return Footer{.metaindex = *metaindex, .index = *index};
}

bool PaddingIsZero(ByteView padding) noexcept {
    for (const std::byte value : padding) {
        if (value != std::byte{0}) {
            return false;
        }
    }
    return true;
}

}  // namespace

BlockContents BlockContents::Owned(std::vector<std::byte> contents) noexcept {
    return BlockContents(std::move(contents));
}

BlockContents BlockContents::Owned(DecodedBlockBytes contents) noexcept {
    return BlockContents(std::move(contents));
}

BlockContents BlockContents::Borrowed(ByteView contents) noexcept {
    return BlockContents(contents);
}

ByteView BlockContents::data() const noexcept {
    return std::visit(
        [](const auto& contents) noexcept -> ByteView {
            if constexpr (std::is_same_v<std::remove_cvref_t<decltype(contents)>,
                                         DecodedBlockBytes>) {
                return {contents.data.get(), contents.size};
            } else {
                return ByteView(contents);
            }
        },
        storage_);
}

void AppendBlockHandle(std::vector<std::byte>& output, BlockHandle handle) {
    AppendVarint64(output, handle.offset);
    AppendVarint64(output, handle.size);
}

Result<BlockHandle> ConsumeBlockHandle(ByteView& input) {
    ByteView remaining = input;
    const Result<std::uint64_t> offset = ConsumeVarint64(remaining);
    if (!offset.has_value()) {
        return std::unexpected(Error::Corruption("block handle offset is malformed"));
    }
    const Result<std::uint64_t> size = ConsumeVarint64(remaining);
    if (!size.has_value()) {
        return std::unexpected(Error::Corruption("block handle size is malformed"));
    }
    input = remaining;
    return BlockHandle{.offset = *offset, .size = *size};
}

std::array<std::byte, FooterSize> EncodeFooter(const Footer& footer) {
    std::vector<std::byte> handles;
    AppendBlockHandle(handles, footer.metaindex);
    AppendBlockHandle(handles, footer.index);

    std::array<std::byte, FooterSize> encoded{};
    std::ranges::copy(handles, encoded.begin());
    EncodeFixedTrusted(std::span(encoded).last<sizeof(TableMagicNumber)>(), TableMagicNumber);
    return encoded;
}

Result<Footer> DecodeFooter(std::span<const std::byte, FooterSize> encoded) {
    if (DecodeFixedTrusted<std::uint64_t>(encoded.last<sizeof(TableMagicNumber)>()) !=
        TableMagicNumber) {
        return std::unexpected(Error::Corruption("table footer has a bad magic number"));
    }
    ByteView handles = encoded.first<FooterHandlesSize>();
    Result<Footer> footer = DecodeFooterHandles(handles);
    if (!footer.has_value()) {
        return std::unexpected(std::move(footer).error());
    }
    if (!PaddingIsZero(handles)) {
        return std::unexpected(Error::Corruption("table footer has nonzero padding"));
    }
    return *footer;
}

std::array<std::byte, BlockTrailerSize> EncodeBlockTrailer(ByteView contents,
                                                           BlockCompression type) noexcept {
    std::array<std::byte, BlockTrailerSize> trailer{};
    trailer[0] = static_cast<std::byte>(type);
    EncodeFixedTrusted(std::span(trailer).last<sizeof(std::uint32_t)>(),
                       BlockChecksum(contents, trailer[0]));
    return trailer;
}

Result<BlockContents> DecodeStoredBlock(std::vector<std::byte> stored) {
    const Result<StoredBlock> decoded = ValidateStoredBlock(stored);
    if (!decoded.has_value()) {
        return std::unexpected(decoded.error());
    }
    if (decoded->compression == BlockCompression::None) {
        stored.resize(decoded->contents.size());
        return BlockContents::Owned(std::move(stored));
    }
    Result<DecodedBlockBytes> decompressed =
        DecompressBlock(decoded->contents, decoded->compression);
    if (!decompressed.has_value()) {
        return std::unexpected(std::move(decompressed).error());
    }
    return BlockContents::Owned(std::move(*decompressed));
}

Result<BlockContents> DecodeStoredBlock(ByteView stored) {
    const Result<StoredBlock> decoded = ValidateStoredBlock(stored);
    if (!decoded.has_value()) {
        return std::unexpected(decoded.error());
    }
    if (decoded->compression == BlockCompression::None) {
        return BlockContents::Borrowed(decoded->contents);
    }
    Result<DecodedBlockBytes> decompressed =
        DecompressBlock(decoded->contents, decoded->compression);
    if (!decompressed.has_value()) {
        return std::unexpected(std::move(decompressed).error());
    }
    return BlockContents::Owned(std::move(*decompressed));
}

}  // namespace modern_leveldb
