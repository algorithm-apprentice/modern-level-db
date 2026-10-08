#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "format/internal_key.h"
#include "modern_leveldb/base/coding.h"
#include "support/native_file_system.h"
#include "support/temporary_directory.h"
#include "table/block.h"
#include "table/compression.h"
#include "table/table.h"
#include "table/table_builder.h"

namespace modern_leveldb {
namespace {

TEST(TableNativeTest, CopiedTablesDecodeEveryCompressionAndKeepPinnedBlocksAlive) {
  for (const auto compression :
       {BlockCompression::None, BlockCompression::Snappy, BlockCompression::Zstd}) {
    SCOPED_TRACE(static_cast<int>(compression));
    test_support::TemporaryDirectory directory;
    const auto path = directory.path() / "table.ldb";
    auto file_system = test_support::CopiedFileSystem();
    const InternalKeyComparator comparator(BytewiseComparator());
    const std::string expected(4'096, 'x');
    auto writable = file_system.OpenWritable(path);
    ASSERT_TRUE(writable.has_value());
    TableBuilderOptions write_options;
    write_options.compression = compression;
    TableBuilder builder(std::move(*writable), comparator, write_options);
    const auto key = InternalKey::Create(AsBytes("key"), 1, ValueKind::Value);
    ASSERT_TRUE(key.has_value());
    ASSERT_TRUE(builder.Add(key->encoded(), AsBytes(expected)).has_value());
    ASSERT_TRUE(builder.Finish().has_value());
    auto file = file_system.OpenRandomAccess(path, builder.file_size());
    ASSERT_TRUE(file.has_value());
    EXPECT_FALSE((*file)->TryReadView(0, 1).has_value());
    BlockCache cache(1U << 20U);
    const std::uint64_t cache_id = cache.NewId() + 1;
    TableOptions read_options;
    read_options.block_cache = &cache;
    auto opened = Table::Open(std::move(*file), builder.file_size(), comparator, read_options);
    ASSERT_TRUE(opened.has_value()) << opened.error().ToString();
    auto& table = **opened;
    const auto lookup = LookupKey::Create(AsBytes("key"), MaxSequenceNumber);
    ASSERT_TRUE(lookup.has_value());
    std::vector<std::byte> value;
    ASSERT_EQ(table.Get(*lookup, value).value(), TableLookupKind::Value);
    EXPECT_EQ(AsStringView(value), expected);
    std::array<std::byte, 16> cache_key{};
    EncodeFixed64(std::span(cache_key).first<8>(), cache_id);
    EncodeFixed64(std::span(cache_key).last<8>(), 0);
    auto pinned = cache.Lookup(cache_key);
    ASSERT_TRUE(pinned.has_value());
    opened->reset();
    ASSERT_TRUE(file_system.RemoveFile(path).has_value());
    Block::Iterator entry(pinned->value(), comparator);
    ASSERT_TRUE(entry.SeekToFirst().has_value());
    ASSERT_TRUE(entry.valid());
    EXPECT_EQ(AsStringView(entry.value()), expected);
  }
}

TEST(TableNativeTest, TruncatedCopiedFilesReportCorruption) {
  test_support::TemporaryDirectory directory;
  const auto path = directory.path() / "table.ldb";
  auto file_system = test_support::CopiedFileSystem();
  const InternalKeyComparator comparator(BytewiseComparator());
  auto writable = file_system.OpenWritable(path);
  ASSERT_TRUE(writable.has_value());
  TableBuilder builder(std::move(*writable), comparator, {});
  const auto key = InternalKey::Create(AsBytes("key"), 1, ValueKind::Value);
  ASSERT_TRUE(key.has_value());
  ASSERT_TRUE(builder.Add(key->encoded(), AsBytes("value")).has_value());
  ASSERT_TRUE(builder.Finish().has_value());
  const std::uint64_t size = builder.file_size();
  std::filesystem::resize_file(path, size - 1);
  auto file = file_system.OpenRandomAccess(path, size);
  ASSERT_TRUE(file.has_value());
  const auto opened = Table::Open(std::move(*file), size, comparator, {});
  ASSERT_FALSE(opened.has_value());
  EXPECT_EQ(opened.error().code(), ErrorCode::Corruption);
}

}  // namespace
}  // namespace modern_leveldb
