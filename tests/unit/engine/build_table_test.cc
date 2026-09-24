#include "engine/build_table.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "engine/table_cache.h"
#include "format/internal_key.h"
#include "memory/memtable.h"
#include "metadata/filenames.h"
#include "metadata/version_edit.h"
#include "modern_leveldb/base/bytes.h"
#include "modern_leveldb/base/comparator.h"
#include "modern_leveldb/base/result.h"
#include "platform/file_system.h"
#include "support/memory_file_system.h"
#include "table/bloom_filter.h"
#include "table/table.h"
#include "table/table_builder.h"

namespace modern_leveldb {
namespace {

using test_support::MemoryFileSystem;

std::vector<std::byte> Key(std::string_view user_key, SequenceNumber sequence, ValueKind kind) {
  auto key = InternalKey::Create(AsBytes(user_key), sequence, kind);
  EXPECT_TRUE(key.has_value());
  const ByteView encoded = key->encoded();
  return std::vector<std::byte>(encoded.begin(), encoded.end());
}

class VectorWritableFile final : public WritableFile {
 public:
  explicit VectorWritableFile(std::vector<std::byte>& data) : data_(data) {}
  Status Append(ByteView data) override {
    data_.insert(data_.end(), data.begin(), data.end());
    return {};
  }
  Status Flush() override { return {}; }
  Status Sync() override { return {}; }
  Status Close() override { return {}; }

 private:
  std::vector<std::byte>& data_;
};

class BuildTableTest : public testing::Test {
 protected:
  BuildTableTest() {
    options_.block_size = 1;
    options_.filter_policy = BloomFilterPolicy(10);
    EXPECT_TRUE(memtable_.Add(3, ValueKind::Value, AsBytes("b"), AsBytes("beta")).has_value());
    EXPECT_TRUE(memtable_.Add(5, ValueKind::Deletion, AsBytes("a"), {}).has_value());
    EXPECT_TRUE(memtable_.Add(1, ValueKind::Value, AsBytes("a"), AsBytes("alpha")).has_value());
    EXPECT_TRUE(memtable_.Add(4, ValueKind::Value, AsBytes("c"), AsBytes("gamma")).has_value());
  }

  Result<std::optional<FileMetadata>> Build(MemoryFileSystem& file_system, TableCache& cache,
                                            const MemTable& memtable) {
    return BuildTable(file_system, directory_, comparator_, options_, cache, memtable, 7);
  }

  // The table that TableBuilder writes for the memtable's entries.
  std::vector<std::byte> ExpectedTable() {
    std::vector<std::byte> data;
    TableBuilder builder(std::make_unique<VectorWritableFile>(data), comparator_, options_);
    MemTable::Iterator entry(memtable_);
    for (entry.SeekToFirst(); entry.valid(); entry.Next()) {
      EXPECT_TRUE(builder.Add(entry.key(), entry.value()).has_value());
    }
    EXPECT_TRUE(builder.Finish().has_value());
    return data;
  }

  TableCache Cache(MemoryFileSystem& file_system) {
    TableOptions table_options;
    table_options.filter_policy = options_.filter_policy;
    return TableCache(file_system, directory_, comparator_, table_options, 10);
  }

  const std::filesystem::path directory_ = std::filesystem::path("db");
  const std::filesystem::path table_ = directory_ / "000007.ldb";
  InternalKeyComparator comparator_{BytewiseComparator()};
  TableBuilderOptions options_;
  MemTable memtable_{BytewiseComparator()};
};

TEST_F(BuildTableTest, WritesAVerifiedTable) {
  MemoryFileSystem file_system;
  TableCache cache(file_system, directory_, comparator_, {}, 10);

  const auto built = Build(file_system, cache, memtable_);

  ASSERT_TRUE(built.has_value()) << built.error().ToString();
  ASSERT_TRUE(built->has_value());
  const FileMetadata& file = **built;
  const std::vector<std::byte> expected = ExpectedTable();
  EXPECT_EQ(file.number, 7U);
  EXPECT_EQ(file.file_size, expected.size());
  EXPECT_EQ(file_system.Contents(table_), expected);
  EXPECT_EQ(comparator_.Compare(file.smallest.encoded(), Key("a", 5, ValueKind::Deletion)), 0);
  EXPECT_EQ(comparator_.Compare(file.largest.encoded(), Key("c", 4, ValueKind::Value)), 0);

  const std::vector<std::string>& operations = file_system.operations();
  ASSERT_FALSE(operations.empty());
  EXPECT_EQ(operations.front(), "open_writable 000007.ldb");
  const auto synced = std::ranges::find(operations, "sync 000007.ldb");
  const auto closed = std::ranges::find(operations, "close 000007.ldb");
  const auto opened = std::ranges::find(operations, "open_random_access 000007.ldb");
  ASSERT_NE(opened, operations.end());
  EXPECT_LT(synced, closed);
  EXPECT_LT(closed, opened);
  for (const std::string& operation : operations) {
    EXPECT_FALSE(operation.starts_with("sync_directory")) << operation;
    EXPECT_FALSE(operation.starts_with("remove")) << operation;
  }

  // The table is cached, and its entries read back.
  const std::size_t before = operations.size();
  auto table = cache.Find(7, file.file_size);
  ASSERT_TRUE(table.has_value());
  EXPECT_EQ(std::ranges::count(
                std::vector<std::string>(operations.begin() + static_cast<std::ptrdiff_t>(before),
                                         operations.end()),
                "open_random_access 000007.ldb"),
            0);
  auto beta = (*table)->Get(LookupKey::Create(AsBytes("b"), 10).value());
  ASSERT_TRUE(beta.has_value() && beta->has_value());
  EXPECT_EQ(AsStringView((*beta)->value), "beta");
  auto deleted = (*table)->Get(LookupKey::Create(AsBytes("a"), 10).value());
  ASSERT_TRUE(deleted.has_value() && deleted->has_value());
  EXPECT_EQ((*deleted)->kind, ValueKind::Deletion);
}

TEST_F(BuildTableTest, WritesNothingForAnEmptyMemtable) {
  MemoryFileSystem file_system;
  TableCache cache = Cache(file_system);
  const MemTable empty(BytewiseComparator());

  const auto built = Build(file_system, cache, empty);

  ASSERT_TRUE(built.has_value());
  EXPECT_FALSE(built->has_value());
  EXPECT_TRUE(file_system.operations().empty());
}

TEST_F(BuildTableTest, RemovesTheTableAfterAFailure) {
  MemoryFileSystem reference;
  TableCache reference_cache = Cache(reference);
  ASSERT_TRUE(Build(reference, reference_cache, memtable_).has_value());
  const std::vector<std::string> operations = reference.operations();
  const std::uint64_t size = ExpectedTable().size();

  for (std::size_t failing = 0; failing < operations.size(); ++failing) {
    SCOPED_TRACE(operations[failing]);
    MemoryFileSystem file_system;
    TableCache cache = Cache(file_system);
    file_system.FailOperation(failing, Error::Io("injected failure"));

    const auto built = Build(file_system, cache, memtable_);

    ASSERT_FALSE(built.has_value());
    EXPECT_EQ(built.error().message(), "injected failure");
    EXPECT_FALSE(file_system.Contents(table_).has_value());
    EXPECT_EQ(file_system.operations().back() == "remove 000007.ldb", failing > 0);
    const auto cached = cache.Find(7, size);
    ASSERT_FALSE(cached.has_value());
    EXPECT_EQ(cached.error().code(), ErrorCode::NotFound);
  }
}

TEST_F(BuildTableTest, ReturnsTheOriginalErrorWhenRemovalFails) {
  MemoryFileSystem reference;
  TableCache reference_cache = Cache(reference);
  ASSERT_TRUE(Build(reference, reference_cache, memtable_).has_value());
  const auto sync = std::ranges::find(reference.operations(), "sync 000007.ldb");
  ASSERT_NE(sync, reference.operations().end());
  const auto sync_index = static_cast<std::size_t>(sync - reference.operations().begin());

  MemoryFileSystem file_system;
  TableCache cache = Cache(file_system);
  file_system.FailOperation(sync_index, Error::Io("injected sync failure"));
  // Finish still closes the file, and then the table is removed.
  file_system.FailOperation(sync_index + 2, Error::Io("injected removal failure"));

  const auto built = Build(file_system, cache, memtable_);

  ASSERT_FALSE(built.has_value());
  EXPECT_EQ(built.error().message(), "injected sync failure");
  EXPECT_EQ(file_system.operations().back(), "remove 000007.ldb");
  EXPECT_TRUE(file_system.Contents(table_).has_value());
}

}  // namespace
}  // namespace modern_leveldb
