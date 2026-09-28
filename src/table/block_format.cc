#include "table/block_format.h"

#include <algorithm>
#include <array>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <span>
#include <utility>
#include <vector>

#if MODERN_LEVELDB_READ_DIAGNOSTICS
#include "engine/read_diagnostics.h"
#endif
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

Result<StoredBlock> ValidateStoredBlock(ByteView stored) {
  if (stored.size() < BlockTrailerSize) {
    return std::unexpected(Error::Corruption("stored block is shorter than its trailer"));
  }
  const std::size_t contents_size = stored.size() - BlockTrailerSize;
  const std::byte type = stored[contents_size];
  const std::uint32_t checksum = DecodeFixed32(stored.last<sizeof(std::uint32_t)>());
  if (checksum != BlockChecksum(stored.first(contents_size), type)) {
    return std::unexpected(Error::Corruption("block checksum mismatch"));
  }
  const auto compression = static_cast<BlockCompression>(type);
  if (compression != BlockCompression::None && compression != BlockCompression::Snappy &&
      compression != BlockCompression::Zstd) {
    return std::unexpected(Error::Corruption("block has an unknown compression type"));
  }
  return StoredBlock{
      .contents = stored.first(contents_size),
      .compression = compression,
  };
}

#if MODERN_LEVELDB_READ_DIAGNOSTICS
void RecordStoredBlock(std::size_t bytes) noexcept {
  read_diagnostics::Add(read_diagnostics::Counter::StoredBlocks);
  read_diagnostics::Add(read_diagnostics::Counter::StoredBlockBytes, bytes);
}

void RecordDecodedBlock(std::size_t bytes, bool decompressed) noexcept {
  read_diagnostics::Add(read_diagnostics::Counter::DecodedBlocks);
  read_diagnostics::Add(read_diagnostics::Counter::DecodedBlockBytes, bytes);
  if (decompressed) {
    read_diagnostics::Add(read_diagnostics::Counter::DecompressedBlocks);
  }
}
#endif

}  // namespace

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
  assert(handles.size() <= FooterHandlesSize);

  std::array<std::byte, FooterSize> encoded{};
  std::ranges::copy(handles, encoded.begin());
  EncodeFixed64(std::span(encoded).last<sizeof(TableMagicNumber)>(), TableMagicNumber);
  return encoded;
}

Result<Footer> DecodeFooter(std::span<const std::byte, FooterSize> encoded) {
  if (DecodeFixed64(encoded.last<sizeof(TableMagicNumber)>()) != TableMagicNumber) {
    return std::unexpected(Error::Corruption("table footer has a bad magic number"));
  }
  ByteView handles = encoded.first<FooterHandlesSize>();
  const Result<BlockHandle> metaindex = ConsumeBlockHandle(handles);
  if (!metaindex.has_value()) {
    return std::unexpected(metaindex.error());
  }
  const Result<BlockHandle> index = ConsumeBlockHandle(handles);
  if (!index.has_value()) {
    return std::unexpected(index.error());
  }
  if (std::ranges::any_of(handles, [](std::byte value) { return value != std::byte{0}; })) {
    return std::unexpected(Error::Corruption("table footer has nonzero padding"));
  }
  return Footer{.metaindex = *metaindex, .index = *index};
}

std::array<std::byte, BlockTrailerSize> EncodeBlockTrailer(ByteView contents,
                                                           BlockCompression type) noexcept {
  std::array<std::byte, BlockTrailerSize> trailer{};
  trailer[0] = static_cast<std::byte>(type);
  EncodeFixed32(std::span(trailer).last<sizeof(std::uint32_t)>(),
                BlockChecksum(contents, trailer[0]));
  return trailer;
}

Result<std::vector<std::byte>> DecodeStoredBlock(std::vector<std::byte> stored) {
#if MODERN_LEVELDB_READ_DIAGNOSTICS
  read_diagnostics::StageScope decode(read_diagnostics::Stage::StoredBlockDecode);
  RecordStoredBlock(stored.size());
#endif
  const Result<StoredBlock> decoded = ValidateStoredBlock(stored);
  if (!decoded.has_value()) {
    return std::unexpected(decoded.error());
  }
  if (decoded->compression == BlockCompression::None) {
    stored.resize(decoded->contents.size());
#if MODERN_LEVELDB_READ_DIAGNOSTICS
    RecordDecodedBlock(stored.size(), false);
#endif
    return stored;
  }
  Result<std::vector<std::byte>> decompressed =
      DecompressBlock(decoded->contents, decoded->compression);
#if MODERN_LEVELDB_READ_DIAGNOSTICS
  if (decompressed.has_value()) {
    RecordDecodedBlock(decompressed->size(), true);
  }
#endif
  return decompressed;
}

Result<std::vector<std::byte>> DecodeStoredBlock(ByteView stored) {
#if MODERN_LEVELDB_READ_DIAGNOSTICS
  read_diagnostics::StageScope decode(read_diagnostics::Stage::StoredBlockDecode);
  RecordStoredBlock(stored.size());
#endif
  const Result<StoredBlock> decoded = ValidateStoredBlock(stored);
  if (!decoded.has_value()) {
    return std::unexpected(decoded.error());
  }
  if (decoded->compression == BlockCompression::None) {
    std::vector<std::byte> owned(decoded->contents.begin(), decoded->contents.end());
#if MODERN_LEVELDB_READ_DIAGNOSTICS
    RecordDecodedBlock(owned.size(), false);
#endif
    return owned;
  }
  Result<std::vector<std::byte>> decompressed =
      DecompressBlock(decoded->contents, decoded->compression);
#if MODERN_LEVELDB_READ_DIAGNOSTICS
  if (decompressed.has_value()) {
    RecordDecodedBlock(decompressed->size(), true);
  }
#endif
  return decompressed;
}

}  // namespace modern_leveldb
