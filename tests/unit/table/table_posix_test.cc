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

BuiltTable BuildTableFile(const std::filesystem::path& path,
                          const InternalKeyComparator& comparator, BlockCompression compression,
                          ByteView value) {
    PosixFileSystem file_system;
    auto file = file_system.OpenWritable(path);
    EXPECT_TRUE(file.has_value()) << file.error().ToString();
    TableBuilderOptions options;
    options.compression = compression;
    TableBuilder builder(std::move(*file), comparator, options);
    const auto key = InternalKey::Create(AsBytes("key"), 1, ValueKind::Value);
    EXPECT_TRUE(key.has_value());
    EXPECT_TRUE(builder.Add(key->encoded(), value).has_value());
    EXPECT_TRUE(builder.Finish().has_value());

    const std::vector<std::byte> bytes = ReadFile(path);
    EXPECT_EQ(bytes.size(), builder.file_size());
    const auto footer_bytes = std::span<const std::byte, FooterSize>(
        bytes.data() + bytes.size() - FooterSize, FooterSize);
    const Result<Footer> footer = DecodeFooter(footer_bytes);
    EXPECT_TRUE(footer.has_value()) << footer.error().ToString();
    const BlockHandle index_handle = footer->index;
    const std::size_t stored_index_size =
        static_cast<std::size_t>(index_handle.size) + BlockTrailerSize;
    const ByteView stored_index(bytes.data() + index_handle.offset, stored_index_size);
    Result<BlockContents> index_contents = DecodeStoredBlock(stored_index);
    EXPECT_TRUE(index_contents.has_value()) << index_contents.error().ToString();
    const Result<Block> index = Block::Create(std::move(*index_contents));
    EXPECT_TRUE(index.has_value()) << index.error().ToString();
    Block::Iterator entry(*index, comparator);
    EXPECT_TRUE(entry.SeekToFirst().has_value());
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
    const BuiltTable built =
        BuildTableFile(path, comparator, BlockCompression::None, AsBytes("value"));
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

TEST(TablePosixTest, MappedUncompressedBlockBypassesTheBlockCache) {
    TemporaryDirectory directory;
    const auto path = directory.path() / "table.ldb";
    const InternalKeyComparator comparator(BytewiseComparator());
    const BuiltTable built =
        BuildTableFile(path, comparator, BlockCompression::None, AsBytes("value"));
    PosixFileSystem file_system(true);
    BlockCache cache(1U << 20U);
    const std::uint64_t expected_cache_id = cache.NewId() + 1;
    auto file = file_system.OpenRandomAccess(path, built.size);
    ASSERT_TRUE(file.has_value()) << file.error().ToString();
    TableOptions options;
    options.block_cache = &cache;
    options.use_trusted_internal_key_comparison = true;
    Result<std::unique_ptr<Table>> table =
        Table::Open(std::move(*file), built.size, comparator, options);
    ASSERT_TRUE(table.has_value()) << table.error().ToString();
    const auto lookup = LookupKey::Create(AsBytes("key"), MaxSequenceNumber);
    ASSERT_TRUE(lookup.has_value());
    std::vector<std::byte> value;
    const auto found = (*table)->Get(*lookup, value);
    ASSERT_TRUE(found.has_value() && *found == TableLookupKind::Value);
    EXPECT_EQ(AsStringView(value), "value");

    std::array<std::byte, 2 * sizeof(std::uint64_t)> cache_key{};
    EncodeFixed64(std::span(cache_key).first<sizeof(std::uint64_t)>(), expected_cache_id);
    EncodeFixed64(std::span(cache_key).last<sizeof(std::uint64_t)>(), built.data.offset);
    EXPECT_FALSE(cache.Lookup(cache_key).has_value());
    EXPECT_EQ(cache.total_charge(), 0U);
    const auto repeated = (*table)->Get(*lookup, value);
    ASSERT_TRUE(repeated.has_value() && *repeated == TableLookupKind::Value);
    EXPECT_EQ(AsStringView(value), "value");
    EXPECT_FALSE(cache.Lookup(cache_key).has_value());

    table->reset();
    ASSERT_TRUE(file_system.RemoveFile(path).has_value());
}

TEST(TablePosixTest, CachedCopiedUncompressedBlockOutlivesTable) {
    TemporaryDirectory directory;
    const auto path = directory.path() / "table.ldb";
    const InternalKeyComparator comparator(BytewiseComparator());
    const BuiltTable built =
        BuildTableFile(path, comparator, BlockCompression::None, AsBytes("value"));
    PosixFileSystem file_system(false);
    BlockCache cache(1U << 20U);
    const std::uint64_t expected_cache_id = cache.NewId() + 1;
    auto file = file_system.OpenRandomAccess(path, built.size);
    ASSERT_TRUE(file.has_value()) << file.error().ToString();
    TableOptions options;
    options.block_cache = &cache;
    options.use_trusted_internal_key_comparison = true;
    Result<std::unique_ptr<Table>> table =
        Table::Open(std::move(*file), built.size, comparator, options);
    ASSERT_TRUE(table.has_value()) << table.error().ToString();
    const auto lookup = LookupKey::Create(AsBytes("key"), MaxSequenceNumber);
    ASSERT_TRUE(lookup.has_value());
    std::vector<std::byte> value;
    const auto found = (*table)->Get(*lookup, value);
    ASSERT_TRUE(found.has_value() && *found == TableLookupKind::Value);
    EXPECT_EQ(AsStringView(value), "value");

    std::array<std::byte, 2 * sizeof(std::uint64_t)> cache_key{};
    EncodeFixed64(std::span(cache_key).first<sizeof(std::uint64_t)>(), expected_cache_id);
    EncodeFixed64(std::span(cache_key).last<sizeof(std::uint64_t)>(), built.data.offset);
    std::optional<BlockCache::Handle> cached = cache.Lookup(cache_key);
    ASSERT_TRUE(cached.has_value());

    table->reset();
    ASSERT_TRUE(file_system.RemoveFile(path).has_value());
    Block::Iterator entry(cached->value(), comparator);
    ASSERT_TRUE(entry.SeekToFirst().has_value());
    ASSERT_TRUE(entry.valid());
    EXPECT_EQ(AsStringView(entry.value()), "value");
}

TEST(TablePosixTest, CachedMappedCompressedBlockOutlivesTable) {
    TemporaryDirectory directory;
    const auto path = directory.path() / "table.ldb";
    const InternalKeyComparator comparator(BytewiseComparator());
    const std::vector<std::byte> expected(4096, std::byte{'x'});
    const BuiltTable built = BuildTableFile(path, comparator, BlockCompression::Snappy, expected);
    const std::vector<std::byte> file_bytes = ReadFile(path);
    ASSERT_LT(built.data.offset + built.data.size, file_bytes.size());
    ASSERT_EQ(static_cast<BlockCompression>(file_bytes[built.data.offset + built.data.size]),
              BlockCompression::Snappy);

    PosixFileSystem file_system(true);
    BlockCache cache(1U << 20U);
    const std::uint64_t expected_cache_id = cache.NewId() + 1;
    auto file = file_system.OpenRandomAccess(path, built.size);
    ASSERT_TRUE(file.has_value()) << file.error().ToString();
    TableOptions options;
    options.block_cache = &cache;
    options.use_trusted_internal_key_comparison = true;
    Result<std::unique_ptr<Table>> table =
        Table::Open(std::move(*file), built.size, comparator, options);
    ASSERT_TRUE(table.has_value()) << table.error().ToString();
    const auto lookup = LookupKey::Create(AsBytes("key"), MaxSequenceNumber);
    ASSERT_TRUE(lookup.has_value());
    std::vector<std::byte> value;
    const auto found = (*table)->Get(*lookup, value);
    ASSERT_TRUE(found.has_value() && *found == TableLookupKind::Value);
    EXPECT_EQ(value, expected);

    std::array<std::byte, 2 * sizeof(std::uint64_t)> cache_key{};
    EncodeFixed64(std::span(cache_key).first<sizeof(std::uint64_t)>(), expected_cache_id);
    EncodeFixed64(std::span(cache_key).last<sizeof(std::uint64_t)>(), built.data.offset);
    std::optional<BlockCache::Handle> cached = cache.Lookup(cache_key);
    ASSERT_TRUE(cached.has_value());

    table->reset();
    ASSERT_TRUE(file_system.RemoveFile(path).has_value());
    Block::Iterator entry(cached->value(), comparator);
    ASSERT_TRUE(entry.SeekToFirst().has_value());
    ASSERT_TRUE(entry.valid());
    EXPECT_EQ(std::vector<std::byte>(entry.value().begin(), entry.value().end()), expected);
}

}  // namespace
}  // namespace modern_leveldb
