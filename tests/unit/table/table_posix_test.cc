#include <gtest/gtest.h>
#include <unistd.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

#include "format/internal_key.h"
#include "modern_leveldb/base/bytes.h"
#include "modern_leveldb/base/coding.h"
#include "platform/posix_file_system.h"
#include "table/block.h"
#include "table/block_format.h"
#include "table/compression.h"
#include "table/table.h"
#include "table/table_builder.h"

namespace modern_leveldb {
namespace {

class TemporaryDirectory {
 public:
  TemporaryDirectory() {
    std::string pattern =
        (std::filesystem::temp_directory_path() / "modern-leveldb-table-XXXXXX").string();
    std::vector<char> writable(pattern.begin(), pattern.end());
    writable.push_back('\0');
    char* const created = ::mkdtemp(writable.data());
    if (created == nullptr) {
      throw std::runtime_error("mkdtemp failed");
    }
    path_ = created;
  }

  ~TemporaryDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }

  [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

 private:
  std::filesystem::path path_;
};

std::vector<std::byte> ReadFile(const std::filesystem::path& path) {
  std::ifstream input(path, std::ios::binary);
  EXPECT_TRUE(input.is_open());
  const std::vector<char> characters{std::istreambuf_iterator<char>(input),
                                     std::istreambuf_iterator<char>()};
  std::vector<std::byte> bytes;
  bytes.reserve(characters.size());
  for (const char character : characters) {
    bytes.push_back(static_cast<std::byte>(static_cast<unsigned char>(character)));
  }
  return bytes;
}

struct BuiltTable {
  std::uint64_t size;
  BlockHandle data;
};

BuiltTable BuildUncompressedTable(const std::filesystem::path& path,
                                  const InternalKeyComparator& comparator) {
  PosixFileSystem file_system;
  auto file = file_system.OpenWritable(path);
  EXPECT_TRUE(file.has_value()) << file.error().ToString();
  TableBuilderOptions options;
  options.compression = BlockCompression::None;
  TableBuilder builder(std::move(*file), comparator, options);
  const auto key = InternalKey::Create(AsBytes("key"), 1, ValueKind::Value);
  EXPECT_TRUE(key.has_value());
  EXPECT_TRUE(builder.Add(key->encoded(), AsBytes("value")).has_value());
  EXPECT_TRUE(builder.Finish().has_value());

  const std::vector<std::byte> bytes = ReadFile(path);
  EXPECT_EQ(bytes.size(), builder.file_size());
  const auto footer_bytes =
      std::span<const std::byte, FooterSize>(bytes.data() + bytes.size() - FooterSize, FooterSize);
  const Result<Footer> footer = DecodeFooter(footer_bytes);
  EXPECT_TRUE(footer.has_value()) << footer.error().ToString();
  const BlockHandle index_handle = footer->index;
  const std::size_t stored_index_size =
      static_cast<std::size_t>(index_handle.size) + BlockTrailerSize;
  const ByteView stored_index(bytes.data() + index_handle.offset, stored_index_size);
  const Result<std::vector<std::byte>> index_contents = DecodeStoredBlock(stored_index);
  EXPECT_TRUE(index_contents.has_value()) << index_contents.error().ToString();
  const Result<Block> index = Block::Create(*index_contents, comparator);
  EXPECT_TRUE(index.has_value()) << index.error().ToString();
  Block::Iterator entry(*index);
  entry.SeekToFirst();
  EXPECT_TRUE(entry.valid());
  ByteView encoded_handle = entry.value();
  const Result<BlockHandle> data = ConsumeBlockHandle(encoded_handle);
  EXPECT_TRUE(data.has_value()) << data.error().ToString();
  EXPECT_TRUE(encoded_handle.empty());
  return {.size = builder.file_size(), .data = *data};
}

TEST(TablePosixTest, TruncatedFileFallsBackToTypedCorruption) {
  TemporaryDirectory directory;
  const auto path = directory.path() / "table.ldb";
  const InternalKeyComparator comparator(BytewiseComparator());
  const BuiltTable built = BuildUncompressedTable(path, comparator);
  ASSERT_GT(built.size, 0U);
  ASSERT_EQ(::truncate(path.c_str(), static_cast<off_t>(built.size - 1)), 0);

  PosixFileSystem file_system(true);
  auto file = file_system.OpenRandomAccess(path, built.size);
  ASSERT_TRUE(file.has_value()) << file.error().ToString();
  EXPECT_FALSE((*file)->TryReadView(0, 1).has_value());
  const Result<std::unique_ptr<Table>> table =
      Table::Open(std::move(*file), built.size, comparator, {});
  ASSERT_FALSE(table.has_value());
  EXPECT_EQ(table.error().code(), ErrorCode::Corruption);
}

TEST(TablePosixTest, CachedUncompressedBlockOutlivesMappedTable) {
  TemporaryDirectory directory;
  const auto path = directory.path() / "table.ldb";
  const InternalKeyComparator comparator(BytewiseComparator());
  const BuiltTable built = BuildUncompressedTable(path, comparator);
  PosixFileSystem file_system(true);
  BlockCache cache(1U << 20U);
  const std::uint64_t expected_cache_id = cache.NewId() + 1;
  auto file = file_system.OpenRandomAccess(path, built.size);
  ASSERT_TRUE(file.has_value()) << file.error().ToString();
  TableOptions options;
  options.block_cache = &cache;
  Result<std::unique_ptr<Table>> table =
      Table::Open(std::move(*file), built.size, comparator, options);
  ASSERT_TRUE(table.has_value()) << table.error().ToString();
  const auto lookup = LookupKey::Create(AsBytes("key"), MaxSequenceNumber);
  ASSERT_TRUE(lookup.has_value());
  const auto value = (*table)->Get(*lookup);
  ASSERT_TRUE(value.has_value() && value->has_value());
  EXPECT_EQ(AsStringView((*value)->value), "value");

  std::array<std::byte, 2 * sizeof(std::uint64_t)> cache_key{};
  EncodeFixed64(std::span(cache_key).first<sizeof(std::uint64_t)>(), expected_cache_id);
  EncodeFixed64(std::span(cache_key).last<sizeof(std::uint64_t)>(), built.data.offset);
  std::optional<BlockCache::Handle> cached = cache.Lookup(cache_key);
  ASSERT_TRUE(cached.has_value());

  table->reset();
  ASSERT_TRUE(file_system.RemoveFile(path).has_value());
  Block::Iterator entry(cached->value());
  entry.SeekToFirst();
  ASSERT_TRUE(entry.valid());
  EXPECT_EQ(AsStringView(entry.value()), "value");
}

}  // namespace
}  // namespace modern_leveldb
