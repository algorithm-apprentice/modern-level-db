#include <gtest/gtest.h>
#include <snappy.h>
#include <zstd.h>

#include <algorithm>
#include <cstddef>
#include <limits>
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
};

CodecResults results;

}  // namespace

// This executable replaces only the codec symbols used by compression.cc.
namespace snappy {

std::size_t MaxCompressedLength(std::size_t) { return results.bound; }

void RawCompress(const char*, std::size_t, char* compressed, std::size_t* size) {
  std::fill_n(compressed, results.compressed, 'c');
  *size = results.compressed;
}

bool GetUncompressedLength(const char*, std::size_t, std::size_t* size) {
  *size = static_cast<std::size_t>(results.declared);
  return results.valid_length;
}

bool RawUncompress(const char*, std::size_t, char*) { return results.valid_contents; }

}  // namespace snappy

extern "C" {

std::size_t ZSTD_compressBound(std::size_t) { return results.bound; }
std::size_t ZSTD_compress(void*, std::size_t, const void*, std::size_t, int) {
  return results.compressed;
}
unsigned ZSTD_isError(std::size_t code) { return code == CodecError ? 1U : 0U; }
unsigned long long ZSTD_getFrameContentSize(const void*, std::size_t) { return results.declared; }
std::size_t ZSTD_decompress(void*, std::size_t, const void*, std::size_t) {
  return results.decoded;
}

}  // extern "C"

namespace modern_leveldb {
namespace {

class CompressionFailureTest : public testing::Test {
 protected:
  void SetUp() override { results = {}; }

  template <typename T>
  static void ExpectCorruption(const Result<T>& result) {
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code(), ErrorCode::Corruption);
  }

  std::vector<std::byte> raw_ = std::vector<std::byte>(64, std::byte{'x'});
  std::vector<std::byte> scratch_;
};

TEST_F(CompressionFailureTest, FallsBackWhenTheCompressionBoundIsTooLarge) {
  results.bound = CodecError;
  EXPECT_FALSE(TryCompressBlock(raw_, BlockCompression::Snappy, 1, scratch_));
  EXPECT_TRUE(scratch_.empty());
  EXPECT_FALSE(TryCompressBlock(raw_, BlockCompression::Zstd, 1, scratch_));
  EXPECT_TRUE(scratch_.empty());

  results.bound = CodecError - 1;
  EXPECT_FALSE(TryCompressBlock(raw_, BlockCompression::Zstd, 1, scratch_));
  EXPECT_TRUE(scratch_.empty());
}

TEST_F(CompressionFailureTest, FallsBackWhenZstdCompressionFails) {
  results.compressed = CodecError;
  EXPECT_FALSE(TryCompressBlock(raw_, BlockCompression::Zstd, 1, scratch_));
  EXPECT_TRUE(scratch_.empty());
}

TEST_F(CompressionFailureTest, RejectsAnUnrepresentableSnappyLength) {
  results.declared = CodecError;
  ExpectCorruption(DecompressBlock(raw_, BlockCompression::Snappy));
}

TEST_F(CompressionFailureTest, RejectsZstdFailureAndLengthMismatch) {
  results.decoded = CodecError;
  ExpectCorruption(DecompressBlock(raw_, BlockCompression::Zstd));
  results.decoded = 15;
  ExpectCorruption(DecompressBlock(raw_, BlockCompression::Zstd));
}

}  // namespace
}  // namespace modern_leveldb
