#include <gtest/gtest.h>
#include <snappy.h>
#include <zstd.h>

#include <algorithm>
#include <cstddef>
#include <initializer_list>
#include <limits>
#include <string_view>
#include <vector>

#include "table/compression.h"

namespace {

constexpr std::size_t CodecError = std::numeric_limits<std::size_t>::max();

struct CodecResults {
    std::size_t bound = 32;
    std::size_t compressed = 8;
    std::size_t decoded = 16;
    unsigned long long declared = 16;
    bool valid_length = true;
    bool valid_contents = true;
    void* decoded_output = nullptr;
    int snappy_bound_calls = 0;
    int snappy_compression_calls = 0;
    int snappy_length_calls = 0;
    int snappy_decode_calls = 0;
    int zstd_bound_calls = 0;
    int zstd_compression_calls = 0;
    int zstd_length_calls = 0;
    int zstd_decode_calls = 0;
};

CodecResults results;

}  // namespace

// This executable replaces only the codec symbols used by compression.cc.
namespace snappy {

std::size_t MaxCompressedLength(std::size_t) {
    ++results.snappy_bound_calls;
    return results.bound;
}

void RawCompress(const char*, std::size_t, char* compressed, std::size_t* size) {
    ++results.snappy_compression_calls;
    std::fill_n(compressed, results.compressed, 'c');
    *size = results.compressed;
}

bool GetUncompressedLength(const char*, std::size_t, std::size_t* size) {
    ++results.snappy_length_calls;
    *size = static_cast<std::size_t>(results.declared);
    return results.valid_length;
}

bool RawUncompress(const char*, std::size_t, char* output) {
    ++results.snappy_decode_calls;
    results.decoded_output = output;
    if (results.valid_contents) {
        std::fill_n(output, static_cast<std::size_t>(results.declared), 'd');
    }
    return results.valid_contents;
}

}  // namespace snappy

extern "C" {

std::size_t ZSTD_compressBound(std::size_t) {
    ++results.zstd_bound_calls;
    return results.bound;
}
std::size_t ZSTD_compress(void*, std::size_t, const void*, std::size_t, int) {
    ++results.zstd_compression_calls;
    return results.compressed;
}
unsigned ZSTD_isError(std::size_t code) { return code == CodecError ? 1U : 0U; }
unsigned long long ZSTD_getFrameContentSize(const void*, std::size_t) {
    ++results.zstd_length_calls;
    return results.declared;
}
std::size_t ZSTD_decompress(void* output, std::size_t capacity, const void*, std::size_t) {
    ++results.zstd_decode_calls;
    results.decoded_output = output;
    if (results.decoded != CodecError) {
        std::fill_n(static_cast<char*>(output), std::min(capacity, results.decoded), 'd');
    }
    return results.decoded;
}

}  // extern "C"

namespace modern_leveldb {
namespace {

class CompressionFailureTest : public testing::Test {
protected:
    void SetUp() override { results = {}; }

    template <typename T>
    static void ExpectCorruption(const Result<T>& result, std::string_view message) {
        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), ErrorCode::Corruption);
        EXPECT_EQ(result.error().message(), message);
    }

    std::vector<std::byte> raw_ = std::vector<std::byte>(64, std::byte{'x'});
    std::vector<std::byte> scratch_;
};

TEST_F(CompressionFailureTest, FallsBackWhenTheCompressionBoundIsTooLarge) {
    results.bound = CodecError;
    scratch_.assign(10, std::byte{0xff});
    EXPECT_FALSE(TryCompressBlock(raw_, BlockCompression::Snappy, 1, scratch_));
    EXPECT_TRUE(scratch_.empty());
    EXPECT_EQ(results.snappy_bound_calls, 1);
    EXPECT_EQ(results.snappy_compression_calls, 0);
    scratch_.assign(10, std::byte{0xff});
    EXPECT_FALSE(TryCompressBlock(raw_, BlockCompression::Zstd, 1, scratch_));
    EXPECT_TRUE(scratch_.empty());

    results.bound = CodecError - 1;
    scratch_.assign(10, std::byte{0xff});
    EXPECT_FALSE(TryCompressBlock(raw_, BlockCompression::Zstd, 1, scratch_));
    EXPECT_TRUE(scratch_.empty());
    EXPECT_EQ(results.zstd_bound_calls, 2);
    EXPECT_EQ(results.zstd_compression_calls, 0);
}

TEST_F(CompressionFailureTest, FallsBackWhenZstdCompressionFails) {
    results.compressed = CodecError;
    scratch_.assign(10, std::byte{0xff});
    EXPECT_FALSE(TryCompressBlock(raw_, BlockCompression::Zstd, 1, scratch_));
    EXPECT_TRUE(scratch_.empty());
    EXPECT_EQ(results.zstd_bound_calls, 1);
    EXPECT_EQ(results.zstd_compression_calls, 1);
}

TEST_F(CompressionFailureTest, RejectsAnUnrepresentableSnappyLength) {
    results.declared = CodecError;
    results.valid_contents = false;
    ExpectCorruption(DecompressBlock(raw_, BlockCompression::Snappy),
                     "compressed Snappy block is too large");
    EXPECT_EQ(results.snappy_length_calls, 1);
    EXPECT_EQ(results.snappy_decode_calls, 0);
}

TEST_F(CompressionFailureTest, RejectsZstdFailureAndLengthMismatch) {
    results.decoded = CodecError;
    ExpectCorruption(DecompressBlock(raw_, BlockCompression::Zstd),
                     "compressed Zstd block has invalid contents");
    results.decoded = 15;
    ExpectCorruption(DecompressBlock(raw_, BlockCompression::Zstd),
                     "compressed Zstd block has invalid contents");
    EXPECT_EQ(results.zstd_length_calls, 2);
    EXPECT_EQ(results.zstd_decode_calls, 2);
}

TEST_F(CompressionFailureTest, ReturnsTheCodecOutputAllocationWithoutCopyingIt) {
    for (const auto type : {BlockCompression::Snappy, BlockCompression::Zstd}) {
        SCOPED_TRACE(static_cast<int>(type));
        results = {};
        results.declared = 19;
        results.decoded = 19;

        const auto decoded = DecompressBlock(raw_, type);

        ASSERT_TRUE(decoded.has_value()) << decoded.error().ToString();
        EXPECT_EQ(decoded->data.get(), results.decoded_output);
        EXPECT_EQ(decoded->size, 19U);
        const ByteView bytes(decoded->data.get(), decoded->size);
        EXPECT_TRUE(
            std::ranges::all_of(bytes, [](std::byte byte) { return byte == std::byte{'d'}; }));
        EXPECT_EQ(results.snappy_decode_calls, type == BlockCompression::Snappy ? 1 : 0);
        EXPECT_EQ(results.zstd_decode_calls, type == BlockCompression::Zstd ? 1 : 0);
    }
}

TEST_F(CompressionFailureTest, AppliesTheStrictSavingsThresholdAfterEachNativeCodec) {
    const std::size_t threshold = raw_.size() - raw_.size() / 8U;
    for (const BlockCompression type : {BlockCompression::Snappy, BlockCompression::Zstd}) {
        for (const std::size_t size : {threshold, threshold - 1}) {
            SCOPED_TRACE(testing::Message() << static_cast<int>(type) << "/" << size);
            results = {};
            results.bound = raw_.size();
            results.compressed = size;
            scratch_.assign(10, std::byte{0xff});

            const bool accepted = TryCompressBlock(raw_, type, 1, scratch_);

            EXPECT_EQ(accepted, size < threshold);
            EXPECT_EQ(scratch_.size(), accepted ? size : 0U);
            EXPECT_EQ(results.snappy_bound_calls, type == BlockCompression::Snappy ? 1 : 0);
            EXPECT_EQ(results.snappy_compression_calls, type == BlockCompression::Snappy ? 1 : 0);
            EXPECT_EQ(results.zstd_bound_calls, type == BlockCompression::Zstd ? 1 : 0);
            EXPECT_EQ(results.zstd_compression_calls, type == BlockCompression::Zstd ? 1 : 0);
            EXPECT_EQ(results.snappy_length_calls, 0);
            EXPECT_EQ(results.snappy_decode_calls, 0);
            EXPECT_EQ(results.zstd_length_calls, 0);
            EXPECT_EQ(results.zstd_decode_calls, 0);
        }
    }
}

TEST_F(CompressionFailureTest, ValidatesSnappyLengthBeforeContents) {
    results.valid_length = false;
    results.declared = CodecError;
    results.valid_contents = false;
    ExpectCorruption(DecompressBlock(raw_, BlockCompression::Snappy),
                     "compressed Snappy block has an invalid length");
    EXPECT_EQ(results.snappy_length_calls, 1);
    EXPECT_EQ(results.snappy_decode_calls, 0);

    results = {};
    results.valid_contents = false;
    ExpectCorruption(DecompressBlock(raw_, BlockCompression::Snappy),
                     "compressed Snappy block has invalid contents");
    EXPECT_EQ(results.snappy_length_calls, 1);
    EXPECT_EQ(results.snappy_decode_calls, 1);
}

TEST_F(CompressionFailureTest, RejectsInvalidZstdSizesBeforeDecoding) {
    for (const unsigned long long size :
         {0ULL, ZSTD_CONTENTSIZE_UNKNOWN, ZSTD_CONTENTSIZE_ERROR, 1ULL << 32}) {
        SCOPED_TRACE(size);
        results = {};
        results.declared = size;
        results.decoded = CodecError;

        ExpectCorruption(DecompressBlock(raw_, BlockCompression::Zstd),
                         "compressed Zstd block has an invalid size");

        EXPECT_EQ(results.zstd_length_calls, 1);
        EXPECT_EQ(results.zstd_decode_calls, 0);
        EXPECT_EQ(results.snappy_length_calls, 0);
        EXPECT_EQ(results.snappy_decode_calls, 0);
    }
}

}  // namespace
}  // namespace modern_leveldb
