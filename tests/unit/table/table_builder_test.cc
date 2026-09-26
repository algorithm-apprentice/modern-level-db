#include "table/table_builder.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <optional>
#include <random>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "format/internal_key.h"
#include "modern_leveldb/base/bytes.h"
#include "modern_leveldb/base/comparator.h"
#include "modern_leveldb/base/result.h"
#include "platform/file_system.h"
#include "table/block.h"
#include "table/block_format.h"
#include "table/bloom_filter.h"
#include "table/compression.h"
#include "table/filter_block.h"

namespace modern_leveldb {
namespace {

static_assert(!std::is_copy_constructible_v<TableBuilder>);
static_assert(!std::is_move_constructible_v<TableBuilder>);
static_assert(!std::is_constructible_v<TableBuilder, std::unique_ptr<WritableFile>,
                                       InternalKeyComparator&&, const TableBuilderOptions&>);

// LevelDB's uncompressed tables for the same inputs, as hex.
constexpr std::string_view EmptyTable =
    "000000000100000000c0f2a1b0000000000100000000c0f2a1b000080d0800000000000000000000"
    "000000000000000000000000000000000000000000000000000057fb808b247547db";
constexpr std::string_view EmptyTableWithFilter =
    "000000000b008ae8dad100220266696c7465722e6c6576656c64622e4275696c74696e426c6f6f6d"
    "46696c74657232000500000000010000000065e85da8000000000100000000c0f2a1b00a2f3e0800"
    "000000000000000000000000000000000000000000000000000000000000000000000057fb808b24"
    "7547db";
constexpr std::string_view ThreeBlockTable =
    "000d016170706c65010300000000000061000000000100000000c8a13075000f0161707269636f74"
    "0102000000000000620000000001000000000deb78a0000e0062616e616e61000100000000000000"
    "0000000100000000170f8fee4245000ca002d00f0600000000090000000b0006536a940022026669"
    "6c7465722e6c6576656c64622e4275696c74696e426c6f6f6d46696c746572325c12000000000100"
    "00000007a37f07000b0261707101ffffffffffffff0019000f0261707269636f7401020000000000"
    "001e1b0009026301ffffffffffffff3e1900000000100000002400000003000000001fe95e41732f"
    "a70142000000000000000000000000000000000000000000000000000000000000000000000057fb"
    "808b247547db";

std::vector<std::byte> FromHex(std::string_view hex) {
  std::vector<std::byte> bytes;
  for (std::size_t index = 0; index + 1 < hex.size(); index += 2) {
    const auto digit = [](char c) {
      return static_cast<unsigned int>(c <= '9' ? c - '0' : c - 'a' + 10);
    };
    bytes.push_back(static_cast<std::byte>(digit(hex[index]) * 16 + digit(hex[index + 1])));
  }
  return bytes;
}

std::vector<std::byte> Materialize(ByteView value) {
  return std::vector<std::byte>(value.begin(), value.end());
}

std::vector<std::byte> Key(std::string_view user_key, SequenceNumber sequence,
                           ValueKind kind = ValueKind::Value) {
  auto key = InternalKey::Create(AsBytes(user_key), sequence, kind);
  EXPECT_TRUE(key.has_value());
  return Materialize(key->encoded());
}

struct WritableState {
  std::vector<std::byte> data;
  int append_calls = 0;
  int flush_calls = 0;
  int sync_calls = 0;
  int close_calls = 0;
  std::optional<int> fail_append_call;
  bool fail_sync = false;
  bool fail_close = false;
};

class TrackingWritableFile final : public WritableFile {
 public:
  explicit TrackingWritableFile(std::shared_ptr<WritableState> state) : state_(std::move(state)) {}

  Status Append(ByteView data) override {
    ++state_->append_calls;
    if (state_->fail_append_call == state_->append_calls) {
      return std::unexpected(Error::Io("injected append failure"));
    }
    state_->data.insert(state_->data.end(), data.begin(), data.end());
    return {};
  }

  Status Flush() override {
    ++state_->flush_calls;
    return {};
  }

  Status Sync() override {
    ++state_->sync_calls;
    if (state_->fail_sync) {
      return std::unexpected(Error::Io("injected sync failure"));
    }
    return {};
  }

  Status Close() override {
    ++state_->close_calls;
    if (state_->fail_close) {
      return std::unexpected(Error::Io("injected close failure"));
    }
    return {};
  }

 private:
  std::shared_ptr<WritableState> state_;
};

class TableBuilderTest : public testing::Test {
 protected:
  std::unique_ptr<TableBuilder> MakeBuilder(const TableBuilderOptions& options = {}) {
    return std::make_unique<TableBuilder>(std::make_unique<TrackingWritableFile>(state_),
                                          comparator_, options);
  }

  static TableBuilderOptions WithFilter(TableBuilderOptions options = {}) {
    options.filter_policy = BloomFilterPolicy(10);
    return options;
  }

  static void ExpectError(const Status& status, ErrorCode code) {
    ASSERT_FALSE(status.has_value());
    EXPECT_EQ(status.error().code(), code) << status.error().ToString();
  }

  std::shared_ptr<WritableState> state_ = std::make_shared<WritableState>();
  InternalKeyComparator comparator_{BytewiseComparator()};
};

struct TableContents {
  std::vector<std::pair<std::vector<std::byte>, std::vector<std::byte>>> entries;
  // The index key and the handle of each data block, and the offset of the
  // data block that holds each entry.
  std::vector<std::vector<std::byte>> index_keys;
  std::vector<BlockHandle> data_handles;
  std::vector<std::uint64_t> entry_block_offsets;
  std::optional<BlockHandle> filter_handle;
  std::optional<FilterBlockReader> filter;
};

BlockCompression StoredCompression(ByteView file, BlockHandle handle) {
  EXPECT_LT(handle.offset + handle.size, file.size());
  return static_cast<BlockCompression>(file[handle.offset + handle.size]);
}

std::vector<std::byte> StoredContents(ByteView file, BlockHandle handle) {
  EXPECT_LE(handle.offset + handle.size + BlockTrailerSize, file.size());
  auto contents =
      DecodeStoredBlock(Materialize(file.subspan(handle.offset, handle.size + BlockTrailerSize)));
  EXPECT_TRUE(contents.has_value());
  return contents.value_or(std::vector<std::byte>{});
}

void ReadTable(ByteView file, const Comparator& comparator, TableContents& table) {
  ASSERT_GE(file.size(), FooterSize);
  const auto footer = DecodeFooter(file.last<FooterSize>());
  ASSERT_TRUE(footer.has_value());

  auto metaindex = Block::Create(StoredContents(file, footer->metaindex), BytewiseComparator());
  ASSERT_TRUE(metaindex.has_value());
  Block::Iterator meta(*metaindex);
  for (meta.SeekToFirst(); meta.valid(); meta.Next()) {
    ASSERT_EQ(AsStringView(meta.key()), "filter.leveldb.BuiltinBloomFilter2");
    ByteView value = meta.value();
    const auto handle = ConsumeBlockHandle(value);
    ASSERT_TRUE(handle.has_value());
    table.filter_handle = *handle;
    auto filter = FilterBlockReader::Create(StoredContents(file, *handle), BloomFilterPolicy(10));
    ASSERT_TRUE(filter.has_value());
    table.filter.emplace(std::move(*filter));
  }

  auto index = Block::Create(StoredContents(file, footer->index), comparator);
  ASSERT_TRUE(index.has_value());
  Block::Iterator index_iterator(*index);
  for (index_iterator.SeekToFirst(); index_iterator.valid(); index_iterator.Next()) {
    table.index_keys.push_back(Materialize(index_iterator.key()));
    ByteView value = index_iterator.value();
    const auto handle = ConsumeBlockHandle(value);
    ASSERT_TRUE(handle.has_value());
    ASSERT_TRUE(value.empty());
    table.data_handles.push_back(*handle);

    auto data = Block::Create(StoredContents(file, *handle), comparator);
    ASSERT_TRUE(data.has_value());
    Block::Iterator entry(*data);
    for (entry.SeekToFirst(); entry.valid(); entry.Next()) {
      table.entries.emplace_back(Materialize(entry.key()), Materialize(entry.value()));
      table.entry_block_offsets.push_back(handle->offset);
    }
  }
}

TEST_F(TableBuilderTest, WritesLevelDbEmptyTables) {
  auto builder = MakeBuilder();
  ASSERT_TRUE(builder->Finish().has_value());

  EXPECT_EQ(state_->data, FromHex(EmptyTable));
  EXPECT_EQ(builder->entry_count(), 0U);
  EXPECT_EQ(builder->file_size(), state_->data.size());

  state_ = std::make_shared<WritableState>();
  auto filtered = MakeBuilder(WithFilter());
  ASSERT_TRUE(filtered->Finish().has_value());

  EXPECT_EQ(state_->data, FromHex(EmptyTableWithFilter));
}

TEST_F(TableBuilderTest, WritesLevelDbTablesWithShortenedIndexKeys) {
  TableBuilderOptions options = WithFilter();
  options.block_size = 1;
  auto builder = MakeBuilder(options);

  ASSERT_TRUE(builder->Add(Key("apple", 3), AsBytes("a")).has_value());
  ASSERT_TRUE(builder->Add(Key("apricot", 2), AsBytes("b")).has_value());
  ASSERT_TRUE(builder->Add(Key("banana", 1, ValueKind::Deletion), {}).has_value());
  ASSERT_TRUE(builder->Finish().has_value());

  EXPECT_EQ(state_->data, FromHex(ThreeBlockTable));
  EXPECT_EQ(builder->entry_count(), 3U);
  EXPECT_EQ(builder->file_size(), state_->data.size());

  TableContents table;
  ASSERT_NO_FATAL_FAILURE(ReadTable(state_->data, comparator_, table));
  ASSERT_EQ(table.index_keys.size(), 3U);
  EXPECT_EQ(table.index_keys[0], Key("apq", MaxSequenceNumber));
  EXPECT_EQ(table.index_keys[1], Key("apricot", 2));
  EXPECT_EQ(table.index_keys[2], Key("c", MaxSequenceNumber));
}

TEST_F(TableBuilderTest, FinishSyncsAndClosesTheFileOnce) {
  auto builder = MakeBuilder();
  ASSERT_TRUE(builder->Add(Key("key", 1), AsBytes("value")).has_value());

  ASSERT_TRUE(builder->Finish().has_value());

  EXPECT_EQ(state_->sync_calls, 1);
  EXPECT_EQ(state_->close_calls, 1);
  EXPECT_EQ(state_->flush_calls, 0);
}

TEST_F(TableBuilderTest, CountsBytesAsBlocksAreWritten) {
  TableBuilderOptions options;
  options.block_size = 64;
  auto builder = MakeBuilder(options);

  EXPECT_EQ(builder->file_size(), 0U);
  for (int index = 0; index < 20; ++index) {
    const std::string user_key = "key" + std::to_string(100 + index);
    ASSERT_TRUE(builder->Add(Key(user_key, 1), AsBytes("value")).has_value());
    EXPECT_EQ(builder->file_size(), state_->data.size());
  }
  EXPECT_GT(builder->file_size(), 0U);
  EXPECT_EQ(builder->entry_count(), 20U);

  ASSERT_TRUE(builder->Finish().has_value());
  EXPECT_EQ(builder->file_size(), state_->data.size());
}

TEST_F(TableBuilderTest, CompressesDataBlocksWithSnappyAndZstd) {
  const std::vector<std::byte> value(16 * 1024, std::byte{'x'});
  for (const BlockCompression compression : {BlockCompression::Snappy, BlockCompression::Zstd}) {
    SCOPED_TRACE(static_cast<int>(compression));
    state_ = std::make_shared<WritableState>();
    TableBuilderOptions options = WithFilter();
    options.block_size = 1 << 20;
    options.compression = compression;
    auto builder = MakeBuilder(options);
    ASSERT_TRUE(builder->Add(Key("key", 1), value).has_value());
    ASSERT_TRUE(builder->Finish().has_value());

    TableContents table;
    ASSERT_NO_FATAL_FAILURE(ReadTable(state_->data, comparator_, table));
    ASSERT_EQ(table.data_handles.size(), 1U);
    EXPECT_EQ(StoredCompression(state_->data, table.data_handles.front()), compression);
    ASSERT_EQ(table.entries.size(), 1U);
    EXPECT_EQ(table.entries.front().second, value);
  }
}

TEST_F(TableBuilderTest, LeavesFilterBlocksUncompressed) {
  std::mt19937_64 random(20260926);
  std::vector<std::byte> value(128 * 1024);
  for (std::byte& byte : value) {
    byte = static_cast<std::byte>(random());
  }
  TableBuilderOptions options = WithFilter();
  options.block_size = 64 * 1024;
  options.compression = BlockCompression::Snappy;
  auto builder = MakeBuilder(options);
  ASSERT_TRUE(builder->Add(Key("a", 1), value).has_value());
  ASSERT_TRUE(builder->Add(Key("b", 1), {}).has_value());
  ASSERT_TRUE(builder->Finish().has_value());

  TableContents table;
  ASSERT_NO_FATAL_FAILURE(ReadTable(state_->data, comparator_, table));
  ASSERT_TRUE(table.filter_handle.has_value());
  EXPECT_EQ(StoredCompression(state_->data, *table.filter_handle), BlockCompression::None);
}

TEST_F(TableBuilderTest, CompressesLargeIndexBlocks) {
  TableBuilderOptions options;
  options.block_size = 1;
  options.compression = BlockCompression::Snappy;
  auto builder = MakeBuilder(options);
  for (int index = 0; index < 500; ++index) {
    ASSERT_TRUE(builder->Add(Key("key" + std::to_string(1000 + index), 1), {}).has_value());
  }
  ASSERT_TRUE(builder->Finish().has_value());

  const auto footer = DecodeFooter(ByteView(state_->data).last<FooterSize>());
  ASSERT_TRUE(footer.has_value());
  EXPECT_EQ(StoredCompression(state_->data, footer->index), BlockCompression::Snappy);
}

TEST_F(TableBuilderTest, RejectsInvalidCompressionBeforeWriting) {
  for (const BlockCompression compression :
       {static_cast<BlockCompression>(0xff), static_cast<BlockCompression>(3)}) {
    SCOPED_TRACE(static_cast<int>(compression));
    state_ = std::make_shared<WritableState>();
    TableBuilderOptions options;
    options.compression = compression;
    auto builder = MakeBuilder(options);
    ExpectError(builder->Add(Key("a", 1), {}), ErrorCode::InvalidArgument);
    ExpectError(builder->Finish(), ErrorCode::InvalidArgument);
    EXPECT_TRUE(state_->data.empty());
    EXPECT_EQ(state_->close_calls, 1);
  }

  for (const int level : {-6, 23}) {
    SCOPED_TRACE(level);
    state_ = std::make_shared<WritableState>();
    TableBuilderOptions options;
    options.compression = BlockCompression::Zstd;
    options.zstd_compression_level = level;
    auto builder = MakeBuilder(options);
    ExpectError(builder->Add(Key("a", 1), {}), ErrorCode::InvalidArgument);
    ExpectError(builder->Finish(), ErrorCode::InvalidArgument);
    EXPECT_TRUE(state_->data.empty());
    EXPECT_EQ(state_->close_calls, 1);
  }
}

TEST_F(TableBuilderTest, RejectsKeysThatAreNotInternalKeys) {
  auto builder = MakeBuilder();
  ASSERT_TRUE(builder->Add(Key("a", 1), {}).has_value());

  ExpectError(builder->Add(AsBytes("short"), {}), ErrorCode::InvalidArgument);
  std::vector<std::byte> unknown_kind = Key("b", 1);
  unknown_kind[unknown_kind.size() - 8] = std::byte{0x02};
  ExpectError(builder->Add(unknown_kind, {}), ErrorCode::InvalidArgument);
}

TEST_F(TableBuilderTest, RejectsKeysThatDoNotIncrease) {
  auto builder = MakeBuilder();
  ASSERT_TRUE(builder->Add(Key("b", 5), {}).has_value());
  ASSERT_TRUE(builder->Add(Key("b", 4), {}).has_value());

  ExpectError(builder->Add(Key("b", 4), {}), ErrorCode::InvalidArgument);

  auto other = MakeBuilder();
  ASSERT_TRUE(other->Add(Key("b", 5), {}).has_value());
  ExpectError(other->Add(Key("a", 9), {}), ErrorCode::InvalidArgument);
}

TEST_F(TableBuilderTest, KeepsTheFirstErrorAndClosesWithoutWriting) {
  auto builder = MakeBuilder(WithFilter());
  ASSERT_TRUE(builder->Add(Key("b", 1), {}).has_value());
  ExpectError(builder->Add(Key("a", 1), {}), ErrorCode::InvalidArgument);

  ExpectError(builder->Add(Key("c", 1), {}), ErrorCode::InvalidArgument);
  ExpectError(builder->Finish(), ErrorCode::InvalidArgument);

  EXPECT_TRUE(state_->data.empty());
  EXPECT_EQ(state_->sync_calls, 0);
  EXPECT_EQ(state_->close_calls, 1);
}

TEST_F(TableBuilderTest, RejectsFilterBlocksLargerThan4GiB) {
  // At this density the eighth key would need a filter larger than 4 GiB. The
  // builder must fail before generating any filter.
  TableBuilderOptions options;
  options.filter_policy = BloomFilterPolicy(std::numeric_limits<std::uint32_t>::max());
  auto builder = MakeBuilder(options);
  for (int index = 0; index < 7; ++index) {
    ASSERT_TRUE(builder->Add(Key("key" + std::to_string(index), 1), {}).has_value());
  }

  ExpectError(builder->Add(Key("key7", 1), {}), ErrorCode::InvalidArgument);
  ExpectError(builder->Finish(), ErrorCode::InvalidArgument);
  EXPECT_EQ(state_->close_calls, 1);
}

TEST_F(TableBuilderTest, ReportsEveryAppendFailure) {
  TableBuilderOptions options = WithFilter();
  options.block_size = 32;
  const auto add_entries = [&](TableBuilder& builder) {
    std::optional<ErrorCode> first_error;
    for (int index = 0; index < 6; ++index) {
      const Status added = builder.Add(Key("key" + std::to_string(index), 1), AsBytes("value"));
      if (first_error.has_value()) {
        ExpectError(added, *first_error);
      } else if (!added.has_value()) {
        first_error = added.error().code();
      }
    }
    return first_error;
  };

  auto reference = MakeBuilder(options);
  ASSERT_FALSE(add_entries(*reference).has_value());
  ASSERT_TRUE(reference->Finish().has_value());
  const int total_appends = state_->append_calls;
  ASSERT_GT(total_appends, 8);

  for (int failing_append = 1; failing_append <= total_appends; ++failing_append) {
    SCOPED_TRACE(failing_append);
    state_ = std::make_shared<WritableState>();
    state_->fail_append_call = failing_append;
    auto builder = MakeBuilder(options);

    const std::optional<ErrorCode> add_error = add_entries(*builder);
    if (add_error.has_value()) {
      EXPECT_EQ(*add_error, ErrorCode::Io);
    }
    ExpectError(builder->Finish(), ErrorCode::Io);
    ExpectError(builder->Add(Key("zzz", 1), {}), ErrorCode::InvalidArgument);
    EXPECT_EQ(state_->append_calls, failing_append);
    EXPECT_EQ(state_->sync_calls, 0);
    EXPECT_EQ(state_->close_calls, 1);
  }
}

TEST_F(TableBuilderTest, ReportsSyncAndCloseFailures) {
  state_->fail_sync = true;
  auto sync_failure = MakeBuilder();
  ExpectError(sync_failure->Finish(), ErrorCode::Io);
  EXPECT_EQ(state_->close_calls, 1);

  state_ = std::make_shared<WritableState>();
  state_->fail_close = true;
  auto close_failure = MakeBuilder();
  ExpectError(close_failure->Finish(), ErrorCode::Io);

  state_ = std::make_shared<WritableState>();
  state_->fail_sync = true;
  state_->fail_close = true;
  auto both = MakeBuilder();
  const Status finished = both->Finish();
  ASSERT_FALSE(finished.has_value());
  EXPECT_EQ(finished.error().message(), "injected sync failure");
}

TEST_F(TableBuilderTest, RejectsCallsAfterFinish) {
  auto builder = MakeBuilder();
  ASSERT_TRUE(builder->Finish().has_value());

  ExpectError(builder->Add(Key("a", 1), {}), ErrorCode::InvalidArgument);
  ExpectError(builder->Finish(), ErrorCode::InvalidArgument);
  EXPECT_EQ(state_->close_calls, 1);

  state_ = std::make_shared<WritableState>();
  state_->fail_sync = true;
  auto failed = MakeBuilder();
  ExpectError(failed->Finish(), ErrorCode::Io);
  ExpectError(failed->Finish(), ErrorCode::InvalidArgument);
}

TEST_F(TableBuilderTest, RejectsANullFile) {
  TableBuilder builder(nullptr, comparator_, {});

  ExpectError(builder.Add(Key("a", 1), {}), ErrorCode::InvalidArgument);
  ExpectError(builder.Finish(), ErrorCode::InvalidArgument);
}

TEST_F(TableBuilderTest, ReadsBackEveryEntryWithMatchingFiltersAndIndexKeys) {
  std::mt19937_64 random(20260924);
  const auto below = [&](std::size_t bound) {
    return std::uniform_int_distribution<std::size_t>(0, bound - 1)(random);
  };
  for (const std::size_t block_size : {std::size_t{1}, std::size_t{64}, std::size_t{4096}}) {
    for (const std::uint32_t restart_interval : {1U, 2U, 16U}) {
      SCOPED_TRACE(testing::Message() << block_size << "/" << restart_interval);
      std::vector<std::vector<std::byte>> keys;
      for (std::size_t index = below(300); index > 0; --index) {
        std::string user_key;
        for (std::size_t length = below(8); length > 0; --length) {
          user_key.push_back("abc"[below(3)]);
        }
        keys.push_back(
            Key(user_key, below(1000), below(2) == 0 ? ValueKind::Value : ValueKind::Deletion));
      }
      std::ranges::sort(keys, [&](ByteView left, ByteView right) {
        return comparator_.Compare(left, right) < 0;
      });
      const auto duplicates = std::ranges::unique(keys, [&](ByteView left, ByteView right) {
        return comparator_.Compare(left, right) == 0;
      });
      keys.erase(duplicates.begin(), duplicates.end());

      state_ = std::make_shared<WritableState>();
      TableBuilderOptions options = WithFilter();
      options.block_size = block_size;
      options.restart_interval = restart_interval;
      auto builder = MakeBuilder(options);
      std::vector<std::pair<std::vector<std::byte>, std::vector<std::byte>>> expected;
      for (const std::vector<std::byte>& key : keys) {
        const std::vector<std::byte> value(below(20), std::byte{'v'});
        ASSERT_TRUE(builder->Add(key, value).has_value());
        expected.emplace_back(key, value);
      }
      ASSERT_TRUE(builder->Finish().has_value());

      TableContents table;
      ASSERT_NO_FATAL_FAILURE(ReadTable(state_->data, comparator_, table));
      EXPECT_EQ(table.entries, expected);
      ASSERT_TRUE(table.filter.has_value());
      for (std::size_t index = 0; index < table.entries.size(); ++index) {
        const ByteView key = table.entries[index].first;
        EXPECT_TRUE(table.filter->KeyMayMatch(table.entry_block_offsets[index],
                                              key.first(key.size() - InternalKeyTrailerSize)));
      }
      // Each index key separates its block from the next one.
      std::size_t entry = 0;
      for (std::size_t block = 0; block < table.data_handles.size(); ++block) {
        while (entry + 1 < table.entries.size() &&
               table.entry_block_offsets[entry + 1] == table.data_handles[block].offset) {
          ++entry;
        }
        EXPECT_GE(comparator_.Compare(table.index_keys[block], table.entries[entry].first), 0);
        if (entry + 1 < table.entries.size()) {
          EXPECT_LT(comparator_.Compare(table.index_keys[block], table.entries[entry + 1].first),
                    0);
        }
        ++entry;
      }
    }
  }
}

}  // namespace
}  // namespace modern_leveldb
