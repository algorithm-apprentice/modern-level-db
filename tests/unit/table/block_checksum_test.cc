#include <crc32c/crc32c.h>
#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

#include "modern_leveldb/base/bytes.h"
#include "modern_leveldb/base/crc32c.h"
#include "support/crc32c_reference.h"
#include "table/block_format.h"

namespace {

struct CrcCall {
    std::uint32_t initial;
    std::vector<std::byte> bytes;
};

std::vector<CrcCall> calls;

}  // namespace

// Count provider calls while retaining an independent, correct CRC32C implementation.
namespace crc32c {

std::uint32_t Extend(std::uint32_t initial, const std::uint8_t* data, std::size_t size) {
    const modern_leveldb::ByteView bytes(reinterpret_cast<const std::byte*>(data), size);
    calls.push_back({.initial = initial, .bytes = {bytes.begin(), bytes.end()}});
    return modern_leveldb::test_support::BitwiseExtend(initial, bytes);
}

}  // namespace crc32c

namespace modern_leveldb {
namespace {

TEST(BlockChecksumCallTest, ReadsContiguousContentsAndTypeWithOneProviderCall) {
    const std::vector<std::vector<std::byte>> inputs{
        {}, {std::byte{'a'}, std::byte{0}, std::byte{0xff}}};
    for (const auto& contents : inputs) {
        for (const bool owning : {false, true}) {
            SCOPED_TRACE(contents.size());
            SCOPED_TRACE(owning);
            std::vector<std::byte> stored(contents);
            const auto trailer = EncodeBlockTrailer(contents, BlockCompression::None);
            stored.insert(stored.end(), trailer.begin(), trailer.end());
            std::vector<std::byte> typed(contents);
            typed.push_back(std::byte{0});
            calls.clear();

            const auto decoded =
                owning ? DecodeStoredBlock(std::move(stored)) : DecodeStoredBlock(ByteView(stored));

            ASSERT_TRUE(decoded.has_value()) << decoded.error().ToString();
            EXPECT_EQ(std::vector(decoded->data().begin(), decoded->data().end()), contents);
            ASSERT_EQ(calls.size(), 1U);
            EXPECT_EQ(calls.front().initial, 0U);
            EXPECT_EQ(calls.front().bytes, typed);
        }
    }
}

TEST(BlockChecksumCallTest, WriteTrailerKeepsIncrementalPayloadAndTypeCalls) {
    const ByteView contents = AsBytes("payload");
    calls.clear();

    const auto trailer = EncodeBlockTrailer(contents, BlockCompression::Snappy);

    ASSERT_EQ(calls.size(), 2U);
    EXPECT_EQ(calls[0].initial, 0U);
    EXPECT_EQ(calls[0].bytes, std::vector(contents.begin(), contents.end()));
    EXPECT_EQ(calls[1].initial, test_support::BitwiseExtend(0, contents));
    EXPECT_EQ(calls[1].bytes, (std::vector{std::byte{1}}));
    EXPECT_EQ(trailer.front(), std::byte{1});
}

}  // namespace
}  // namespace modern_leveldb
