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

#include "modern_leveldb/base/bytes.h"
#include "modern_leveldb/base/coding.h"
#include "modern_leveldb/base/crc32c.h"
#include "modern_leveldb/base/result.h"

namespace modern_leveldb {
namespace {

constexpr std::size_t FooterHandlesSize = 2 * BlockHandleMaxEncodedSize;

constexpr std::byte NoCompression{0x00};
constexpr std::byte SnappyCompression{0x01};
constexpr std::byte ZstdCompression{0x02};

std::uint32_t BlockChecksum(ByteView contents, std::byte type) noexcept {
  return MaskCrc32c(ExtendCrc32c(Crc32c(contents), ByteView(&type, 1)));
}

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

std::array<std::byte, BlockTrailerSize> EncodeBlockTrailer(ByteView contents) noexcept {
  std::array<std::byte, BlockTrailerSize> trailer{};
  trailer[0] = NoCompression;
  EncodeFixed32(std::span(trailer).last<sizeof(std::uint32_t)>(),
                BlockChecksum(contents, NoCompression));
  return trailer;
}

Result<std::vector<std::byte>> DecodeStoredBlock(std::vector<std::byte> stored) {
  if (stored.size() < BlockTrailerSize) {
    return std::unexpected(Error::Corruption("stored block is shorter than its trailer"));
  }
  const std::size_t contents_size = stored.size() - BlockTrailerSize;
  const ByteView view = stored;
  const std::byte type = view[contents_size];
  const std::uint32_t checksum = DecodeFixed32(view.last<sizeof(std::uint32_t)>());
  if (checksum != BlockChecksum(view.first(contents_size), type)) {
    return std::unexpected(Error::Corruption("block checksum mismatch"));
  }
  if (type == SnappyCompression || type == ZstdCompression) {
    return std::unexpected(Error::NotSupported("compressed blocks are not supported"));
  }
  if (type != NoCompression) {
    return std::unexpected(Error::Corruption("block has an unknown compression type"));
  }
  stored.resize(contents_size);
  return stored;
}

}  // namespace modern_leveldb
