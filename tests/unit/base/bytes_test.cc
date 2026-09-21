#include "modern_leveldb/base/bytes.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <string>
#include <string_view>

namespace modern_leveldb {
namespace {

TEST(BytesTest, CreatesEmptyView) {
  const ByteView bytes = AsBytes(std::string_view{});

  EXPECT_TRUE(bytes.empty());
  EXPECT_EQ(bytes.size(), 0U);
}

TEST(BytesTest, PreservesEmbeddedNullBytes) {
  const std::string value{"a\0b", 3};
  const ByteView bytes = AsBytes(value);

  ASSERT_EQ(bytes.size(), 3U);
  EXPECT_EQ(bytes[0], std::byte{'a'});
  EXPECT_EQ(bytes[1], std::byte{0});
  EXPECT_EQ(bytes[2], std::byte{'b'});
}

TEST(BytesTest, IsANonOwningView) {
  std::string value = "abc";
  const ByteView bytes = AsBytes(value);

  value[1] = 'x';

  EXPECT_EQ(bytes[1], std::byte{'x'});
}

TEST(BytesTest, ConvertsBackToStringViewWithoutCopying) {
  const std::string value{"key\0suffix", 10};
  const ByteView bytes = AsBytes(value);
  const std::string_view text = AsStringView(bytes);

  EXPECT_EQ(text, std::string_view(value));
  EXPECT_EQ(text.data(), value.data());
}

TEST(BytesTest, MutableViewUpdatesOriginalStorage) {
  std::array<char, 3> value{'a', 'b', 'c'};
  MutableByteView bytes = AsWritableBytes(value);

  bytes[1] = std::byte{'x'};

  EXPECT_EQ(value, (std::array<char, 3>{'a', 'x', 'c'}));
}

}  // namespace
}  // namespace modern_leveldb
