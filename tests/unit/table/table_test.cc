#include "table/table.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "cache/sharded_lru_cache.h"
#include "format/internal_key.h"
#include "modern_leveldb/base/bytes.h"
#include "modern_leveldb/base/coding.h"
#include "modern_leveldb/base/comparator.h"
#include "modern_leveldb/base/crc32c.h"
#include "modern_leveldb/base/result.h"
#include "platform/file_system.h"
#include "table/block.h"
#include "table/block_builder.h"
#include "table/block_format.h"
#include "table/bloom_filter.h"
#include "table/table_builder.h"

namespace modern_leveldb {
namespace {

template <typename T>
concept OpenableWith = requires(std::unique_ptr<RandomAccessFile> file, T&& comparator) {
  Table::Open(std::move(file), 0, std::forward<T>(comparator), TableOptions{});
};

static_assert(!std::is_copy_constructible_v<Table>);
static_assert(!std::is_move_constructible_v<Table>);
static_assert(!std::is_copy_constructible_v<Table::Iterator>);
static_assert(!std::is_move_constructible_v<Table::Iterator>);
static_assert(!std::is_constructible_v<Table::Iterator, Table&&>);
static_assert(OpenableWith<const InternalKeyComparator&>);
static_assert(!OpenableWith<InternalKeyComparator>);

// LevelDB's tables from the writer tests, as hex.
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

class CaseInsensitiveComparator final : public Comparator {
 public:
  int Compare(ByteView left, ByteView right) const noexcept override {
    const std::size_t common = std::min(left.size(), right.size());
    for (std::size_t index = 0; index < common; ++index) {
      const unsigned int left_byte = Fold(left[index]);
      const unsigned int right_byte = Fold(right[index]);
      if (left_byte != right_byte) {
        return left_byte < right_byte ? -1 : 1;
      }
    }
    return left.size() == right.size() ? 0 : (left.size() < right.size() ? -1 : 1);
  }
  std::string_view Name() const noexcept override { return "test.CaseInsensitive"; }
  void FindShortestSeparator(std::vector<std::byte>&, ByteView) const override {}
  void FindShortSuccessor(std::vector<std::byte>&) const override {}

 private:
  static unsigned int Fold(std::byte value) noexcept {
    const auto byte = std::to_integer<unsigned int>(value);
    return byte >= 'A' && byte <= 'Z' ? byte + ('a' - 'A') : byte;
  }
};

struct ReadableState {
  std::vector<std::byte> data;
  std::atomic<int> read_calls = 0;
  std::optional<int> fail_read_call;
  std::size_t maximum_chunk = std::numeric_limits<std::size_t>::max();
  bool return_oversized_count = false;
};

class MemoryRandomAccessFile final : public RandomAccessFile {
 public:
  explicit MemoryRandomAccessFile(std::shared_ptr<ReadableState> state)
      : state_(std::move(state)) {}

  Result<std::size_t> Read(std::uint64_t offset, MutableByteView output) const override {
    const int call = ++state_->read_calls;
    if (state_->fail_read_call == call) {
      return std::unexpected(Error::Io("injected read failure"));
    }
    if (state_->return_oversized_count) {
      return output.size() + 1;
    }
    if (offset >= state_->data.size()) {
      return 0;
    }
    const std::size_t count =
        std::min({output.size(), state_->data.size() - static_cast<std::size_t>(offset),
                  state_->maximum_chunk});
    std::copy_n(state_->data.begin() + static_cast<std::ptrdiff_t>(offset), count, output.begin());
    return count;
  }

 private:
  std::shared_ptr<ReadableState> state_;
};

class MemoryWritableFile final : public WritableFile {
 public:
  explicit MemoryWritableFile(std::vector<std::byte>& data) : data_(data) {}
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

struct Entry {
  std::vector<std::byte> key;
  std::vector<std::byte> value;

  friend bool operator==(const Entry&, const Entry&) = default;
};

std::vector<std::byte> BuildTable(const std::vector<Entry>& entries,
                                  const TableBuilderOptions& options,
                                  const InternalKeyComparator& comparator) {
  std::vector<std::byte> data;
  TableBuilder builder(std::make_unique<MemoryWritableFile>(data), comparator, options);
  for (const Entry& entry : entries) {
    EXPECT_TRUE(builder.Add(entry.key, entry.value).has_value());
  }
  EXPECT_TRUE(builder.Finish().has_value());
  return data;
}

// Assembles tables block by block, so that tests can store blocks with valid
// checksums that no writer produces.
class TableAssembler {
 public:
  BlockHandle AddBlock(ByteView contents, unsigned int type = 0) {
    const BlockHandle handle{.offset = data_.size(), .size = contents.size()};
    data_.insert(data_.end(), contents.begin(), contents.end());
    data_.push_back(static_cast<std::byte>(type));
    const ByteView typed = ByteView(data_).last(contents.size() + 1);
    AppendFixed32(data_, MaskCrc32c(Crc32c(typed)));
    return handle;
  }

  std::vector<std::byte> Finish(BlockHandle metaindex, BlockHandle index) {
    const auto footer = EncodeFooter({.metaindex = metaindex, .index = index});
    data_.insert(data_.end(), footer.begin(), footer.end());
    return std::move(data_);
  }

 private:
  std::vector<std::byte> data_;
};

std::vector<std::byte> BlockOf(std::initializer_list<std::pair<ByteView, ByteView>> entries) {
  BlockBuilder builder(1);
  for (const auto& [key, value] : entries) {
    EXPECT_TRUE(builder.Add(key, value).has_value());
  }
  return Materialize(builder.Finish());
}

std::vector<std::byte> HandleOf(BlockHandle handle) {
  std::vector<std::byte> encoded;
  AppendBlockHandle(encoded, handle);
  return encoded;
}

class TableTest : public testing::Test {
 protected:
  Result<std::unique_ptr<Table>> TryOpen(std::vector<std::byte> data,
                                         const TableOptions& options = {},
                                         std::optional<std::uint64_t> reported_size = {}) {
    state_ = std::make_shared<ReadableState>();
    state_->data = std::move(data);
    return Table::Open(std::make_unique<MemoryRandomAccessFile>(state_),
                       reported_size.value_or(state_->data.size()), comparator_, options);
  }

  std::unique_ptr<Table> Open(std::vector<std::byte> data, const TableOptions& options = {}) {
    auto table = TryOpen(std::move(data), options);
    EXPECT_TRUE(table.has_value()) << table.error().ToString();
    return table.has_value() ? std::move(*table) : nullptr;
  }

  static TableOptions WithFilter(TableOptions options = {}) {
    options.filter_policy = BloomFilterPolicy(10);
    return options;
  }

  static Result<std::optional<TableLookup>> TryGet(const Table& table, std::string_view user_key,
                                                   SequenceNumber sequence,
                                                   const TableReadOptions& options = {}) {
    auto key = LookupKey::Create(AsBytes(user_key), sequence);
    EXPECT_TRUE(key.has_value());
    return table.Get(*key, options);
  }

  static std::optional<TableLookup> Get(const Table& table, std::string_view user_key,
                                        SequenceNumber sequence,
                                        const TableReadOptions& options = {}) {
    auto lookup = TryGet(table, user_key, sequence, options);
    EXPECT_TRUE(lookup.has_value()) << lookup.error().ToString();
    return lookup.has_value() ? std::move(*lookup) : std::nullopt;
  }

  static void ExpectValue(const std::optional<TableLookup>& lookup, std::string_view value) {
    ASSERT_TRUE(lookup.has_value());
    EXPECT_EQ(lookup->kind, ValueKind::Value);
    EXPECT_EQ(AsStringView(lookup->value), value);
  }

  template <typename T>
  static void ExpectError(const Result<T>& result, ErrorCode code) {
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code(), code) << result.error().ToString();
  }

  static std::vector<Entry> ScanForward(const Table& table) {
    std::vector<Entry> entries;
    Table::Iterator iterator(table);
    Status status = iterator.SeekToFirst();
    while (status.has_value() && iterator.valid()) {
      entries.push_back({Materialize(iterator.key()), Materialize(iterator.value())});
      status = iterator.Next();
    }
    EXPECT_TRUE(status.has_value());
    return entries;
  }

  static std::vector<Entry> ScanBackward(const Table& table) {
    std::vector<Entry> entries;
    Table::Iterator iterator(table);
    Status status = iterator.SeekToLast();
    while (status.has_value() && iterator.valid()) {
      entries.insert(entries.begin(), {Materialize(iterator.key()), Materialize(iterator.value())});
      status = iterator.Prev();
    }
    EXPECT_TRUE(status.has_value());
    return entries;
  }

  // Entries with one version per block: k@30 = v30, k@20 deleted, k@10 = v10, m@5 = m5.
  std::vector<std::byte> VersionedTable() {
    TableBuilderOptions options;
    options.block_size = 1;
    return BuildTable({{Key("k", 30), Materialize(AsBytes("v30"))},
                       {Key("k", 20, ValueKind::Deletion), {}},
                       {Key("k", 10), Materialize(AsBytes("v10"))},
                       {Key("m", 5), Materialize(AsBytes("m5"))}},
                      options, comparator_);
  }

  InternalKeyComparator comparator_{BytewiseComparator()};
  std::shared_ptr<ReadableState> state_;
};

TEST_F(TableTest, ReadsLevelDbTables) {
  const auto table = Open(FromHex(ThreeBlockTable), WithFilter());
  ASSERT_NE(table, nullptr);

  EXPECT_EQ(ScanForward(*table), (std::vector<Entry>{
                                     {Key("apple", 3), Materialize(AsBytes("a"))},
                                     {Key("apricot", 2), Materialize(AsBytes("b"))},
                                     {Key("banana", 1, ValueKind::Deletion), {}},
                                 }));
  ExpectValue(Get(*table, "apple", MaxSequenceNumber), "a");
  ExpectValue(Get(*table, "apricot", MaxSequenceNumber), "b");
  const auto banana = Get(*table, "banana", MaxSequenceNumber);
  ASSERT_TRUE(banana.has_value());
  EXPECT_EQ(banana->kind, ValueKind::Deletion);
  EXPECT_FALSE(Get(*table, "aardvark", MaxSequenceNumber).has_value());
  EXPECT_FALSE(Get(*table, "cherry", MaxSequenceNumber).has_value());

  const auto empty = Open(FromHex(EmptyTableWithFilter), WithFilter());
  ASSERT_NE(empty, nullptr);
  EXPECT_TRUE(ScanForward(*empty).empty());
  EXPECT_FALSE(Get(*empty, "apple", MaxSequenceNumber).has_value());
}

TEST_F(TableTest, FindsTheVersionVisibleAtTheLookupSequence) {
  const auto table = Open(VersionedTable());
  ASSERT_NE(table, nullptr);

  ExpectValue(Get(*table, "k", 40), "v30");
  ExpectValue(Get(*table, "k", 30), "v30");
  const auto deleted = Get(*table, "k", 29);
  ASSERT_TRUE(deleted.has_value());
  EXPECT_EQ(deleted->kind, ValueKind::Deletion);
  EXPECT_TRUE(deleted->value.empty());
  ExpectValue(Get(*table, "k", 15), "v10");
  EXPECT_FALSE(Get(*table, "k", 9).has_value());
  EXPECT_FALSE(Get(*table, "l", MaxSequenceNumber).has_value());
  EXPECT_FALSE(Get(*table, "m", 4).has_value());
  EXPECT_FALSE(Get(*table, "z", MaxSequenceNumber).has_value());
}

TEST_F(TableTest, ComparesUserKeysWithTheUserComparator) {
  const CaseInsensitiveComparator user_comparator;
  const InternalKeyComparator comparator(user_comparator);
  state_ = std::make_shared<ReadableState>();
  state_->data = BuildTable({{Key("Key", 3), Materialize(AsBytes("value"))}}, {}, comparator);

  auto table = Table::Open(std::make_unique<MemoryRandomAccessFile>(state_), state_->data.size(),
                           comparator, {});
  ASSERT_TRUE(table.has_value());

  ExpectValue(Get(**table, "KEY", MaxSequenceNumber), "value");
  ExpectValue(Get(**table, "key", MaxSequenceNumber), "value");
}

TEST_F(TableTest, IteratesAcrossBlocksInBothDirections) {
  const auto table = Open(FromHex(ThreeBlockTable));
  ASSERT_NE(table, nullptr);
  Table::Iterator iterator(*table);
  EXPECT_FALSE(iterator.valid());

  // "apq" separates the first two blocks, so the seek continues in the second.
  ASSERT_TRUE(iterator.Seek(Key("apq", MaxSequenceNumber)).has_value());
  ASSERT_TRUE(iterator.valid());
  EXPECT_EQ(Materialize(iterator.key()), Key("apricot", 2));
  ASSERT_TRUE(iterator.Prev().has_value());
  EXPECT_EQ(Materialize(iterator.key()), Key("apple", 3));
  ASSERT_TRUE(iterator.Prev().has_value());
  EXPECT_FALSE(iterator.valid());

  ASSERT_TRUE(iterator.Seek(Key("apple", 3)).has_value());
  EXPECT_EQ(Materialize(iterator.key()), Key("apple", 3));
  ASSERT_TRUE(iterator.Seek(Key("zebra", 1)).has_value());
  EXPECT_FALSE(iterator.valid());
  ASSERT_TRUE(iterator.SeekToLast().has_value());
  EXPECT_EQ(Materialize(iterator.key()), Key("banana", 1, ValueKind::Deletion));
  EXPECT_TRUE(iterator.value().empty());
  ASSERT_TRUE(iterator.Next().has_value());
  EXPECT_FALSE(iterator.valid());

  // A lookup reads only the block that the index chooses. "applesauce" sorts
  // before the separator "apq" but after the block's only key, "apple".
  EXPECT_FALSE(Get(*table, "applesauce", MaxSequenceNumber).has_value());
}

TEST_F(TableTest, EmptyTablesHaveNoPositions) {
  const auto table = Open(BuildTable({}, {}, comparator_));
  ASSERT_NE(table, nullptr);
  Table::Iterator iterator(*table);

  ASSERT_TRUE(iterator.SeekToFirst().has_value());
  EXPECT_FALSE(iterator.valid());
  ASSERT_TRUE(iterator.SeekToLast().has_value());
  EXPECT_FALSE(iterator.valid());
  ASSERT_TRUE(iterator.Seek(Key("a", 1)).has_value());
  EXPECT_FALSE(iterator.valid());
}

TEST_F(TableTest, SkipsDataBlocksTheFilterRulesOut) {
  std::vector<Entry> entries;
  for (int index = 0; index < 100; ++index) {
    entries.push_back({Key("key" + std::to_string(1000 + index), 1), {}});
  }
  TableBuilderOptions options;
  options.filter_policy = BloomFilterPolicy(10);
  const std::vector<std::byte> data = BuildTable(entries, options, comparator_);

  const auto filtered = Open(data, WithFilter());
  ASSERT_NE(filtered, nullptr);
  const int reads_after_open = state_->read_calls;
  for (int index = 0; index < 100; ++index) {
    EXPECT_FALSE(Get(*filtered, "absent" + std::to_string(index), 1).has_value());
  }
  // A Bloom filter with ten bits per key has a false-positive rate near 1%.
  EXPECT_LT(state_->read_calls - reads_after_open, 10);
  EXPECT_TRUE(Get(*filtered, "key1050", 1).has_value());

  const auto unfiltered = Open(data);
  ASSERT_NE(unfiltered, nullptr);
  const int unfiltered_reads = state_->read_calls;
  EXPECT_FALSE(Get(*unfiltered, "absent", 1).has_value());
  EXPECT_EQ(state_->read_calls - unfiltered_reads, 1);

  const auto without_filter_block = Open(BuildTable(entries, {}, comparator_), WithFilter());
  ASSERT_NE(without_filter_block, nullptr);
  EXPECT_TRUE(Get(*without_filter_block, "key1050", 1).has_value());
}

TEST_F(TableTest, ServesRepeatedReadsFromTheBlockCache) {
  BlockCache cache(1 << 20);
  TableOptions options;
  options.block_cache = &cache;
  const auto table = Open(VersionedTable(), options);
  ASSERT_NE(table, nullptr);

  const int before = state_->read_calls;
  ExpectValue(Get(*table, "k", 40), "v30");
  EXPECT_EQ(state_->read_calls - before, 1);
  ExpectValue(Get(*table, "k", 40), "v30");
  EXPECT_EQ(state_->read_calls - before, 1);
  EXPECT_GT(cache.total_charge(), 0U);

  static_cast<void>(ScanForward(*table));
  const int after_scan = state_->read_calls;
  static_cast<void>(ScanForward(*table));
  EXPECT_EQ(state_->read_calls, after_scan);
}

TEST_F(TableTest, ReadsWithoutFillingTheCacheWhenAsked) {
  BlockCache cache(1 << 20);
  TableOptions options;
  options.block_cache = &cache;
  const auto table = Open(VersionedTable(), options);
  ASSERT_NE(table, nullptr);
  const TableReadOptions no_fill{.fill_cache = false};

  const int before = state_->read_calls;
  ExpectValue(Get(*table, "k", 40, no_fill), "v30");
  ExpectValue(Get(*table, "k", 40, no_fill), "v30");
  EXPECT_EQ(state_->read_calls - before, 2);
  EXPECT_EQ(cache.total_charge(), 0U);

  ExpectValue(Get(*table, "k", 40), "v30");
  ExpectValue(Get(*table, "k", 40, no_fill), "v30");
  EXPECT_EQ(state_->read_calls - before, 3);
}

TEST_F(TableTest, KeepsTablesApartInASharedCache) {
  BlockCache cache(1 << 20);
  TableOptions options;
  options.block_cache = &cache;
  const auto first =
      Open(BuildTable({{Key("key", 1), Materialize(AsBytes("first"))}}, {}, comparator_), options);
  const auto second =
      Open(BuildTable({{Key("key", 1), Materialize(AsBytes("second"))}}, {}, comparator_), options);
  ASSERT_NE(first, nullptr);
  ASSERT_NE(second, nullptr);

  ExpectValue(Get(*first, "key", 1), "first");
  ExpectValue(Get(*second, "key", 1), "second");
  ExpectValue(Get(*first, "key", 1), "first");
}

TEST_F(TableTest, ReadsBlocksWhoseChargeTheCacheCannotAccount) {
  BlockCache cache(1 << 20);
  // Pinning the largest charge in every shard makes every further insertion
  // overflow the cache's charge accounting.
  const auto placeholder =
      std::make_shared<const Block>(Block::Create(BlockOf({}), BytewiseComparator()).value());
  std::vector<BlockCache::Handle> pins;
  for (int candidate = 0; candidate < 256; ++candidate) {
    const std::string key = "pin-" + std::to_string(candidate);
    auto pin = cache.Insert(AsBytes(key), placeholder, std::numeric_limits<std::size_t>::max());
    if (pin.has_value()) {
      pins.push_back(std::move(*pin));
    }
  }
  TableOptions options;
  options.block_cache = &cache;
  const auto table = Open(VersionedTable(), options);
  ASSERT_NE(table, nullptr);

  const int before = state_->read_calls;
  ExpectValue(Get(*table, "k", 40), "v30");
  ExpectValue(Get(*table, "k", 40), "v30");
  EXPECT_EQ(state_->read_calls - before, 2);
  EXPECT_EQ(ScanForward(*table).size(), 4U);
}

TEST_F(TableTest, KeepsBlocksAliveWithoutCacheCapacity) {
  BlockCache cache(0);
  TableOptions options;
  options.block_cache = &cache;
  const auto table = Open(VersionedTable(), options);
  ASSERT_NE(table, nullptr);

  EXPECT_EQ(ScanForward(*table).size(), 4U);
  EXPECT_EQ(ScanBackward(*table).size(), 4U);
  ExpectValue(Get(*table, "m", 5), "m5");
}

TEST_F(TableTest, KeepsThePositionedBlockWhenTheCacheEvictsIt) {
  BlockCache cache(1);
  TableOptions options;
  options.block_cache = &cache;
  const auto table = Open(VersionedTable(), options);
  const auto other = Open(VersionedTable(), options);
  ASSERT_NE(table, nullptr);
  ASSERT_NE(other, nullptr);
  Table::Iterator iterator(*table);
  ASSERT_TRUE(iterator.SeekToFirst().has_value());

  for (int round = 0; round < 3; ++round) {
    static_cast<void>(ScanForward(*other));
  }

  ASSERT_TRUE(iterator.valid());
  EXPECT_EQ(Materialize(iterator.key()), Key("k", 30));
  EXPECT_EQ(AsStringView(iterator.value()), "v30");
  ASSERT_TRUE(iterator.Next().has_value());
  EXPECT_EQ(Materialize(iterator.key()), Key("k", 20, ValueKind::Deletion));
}

TEST_F(TableTest, RejectsFilesThatAreNotTables) {
  ExpectError(TryOpen(std::vector<std::byte>(FooterSize - 1)), ErrorCode::Corruption);

  std::vector<std::byte> bad_magic = FromHex(ThreeBlockTable);
  bad_magic.back() ^= std::byte{0x01};
  ExpectError(TryOpen(std::move(bad_magic)), ErrorCode::Corruption);

  ExpectError(Table::Open(nullptr, 100, comparator_, {}), ErrorCode::InvalidArgument);
}

TEST_F(TableTest, RejectsDamagedIndexBlocks) {
  std::vector<std::byte> corrupt = FromHex(ThreeBlockTable);
  // The index block starts at offset 167 in this table.
  corrupt[172] ^= std::byte{0x01};
  ExpectError(TryOpen(std::move(corrupt)), ErrorCode::Corruption);

  // The single empty block with its trailer ends at offset 13, before the footer.
  for (const BlockHandle index :
       {BlockHandle{.offset = 14, .size = 0}, BlockHandle{.offset = 5, .size = 100},
        BlockHandle{.offset = 10, .size = 0}}) {
    SCOPED_TRACE(index.offset);
    TableAssembler outside;
    const BlockHandle empty = outside.AddBlock(BlockOf({}));
    ExpectError(TryOpen(outside.Finish(empty, index)), ErrorCode::Corruption);
  }

  TableAssembler compressed;
  const BlockHandle meta = compressed.AddBlock(BlockOf({}));
  ExpectError(TryOpen(compressed.Finish(meta, compressed.AddBlock(BlockOf({}), 1))),
              ErrorCode::NotSupported);
}

TEST_F(TableTest, RejectsIndexValuesThatAreNotBlockHandles) {
  const std::vector<std::byte> index_key = Key("z", 1);
  const auto assemble = [&](ByteView (*value)(BlockHandle, std::vector<std::byte>&)) {
    TableAssembler table;
    const BlockHandle data = table.AddBlock(BlockOf({{Key("a", 1), AsBytes("x")}}));
    std::vector<std::byte> storage;
    const BlockHandle index = table.AddBlock(BlockOf({{index_key, value(data, storage)}}));
    return table.Finish(table.AddBlock(BlockOf({})), index);
  };

  ExpectError(TryOpen(assemble([](BlockHandle, std::vector<std::byte>& storage) {
                storage = {std::byte{0x80}};
                return ByteView(storage);
              })),
              ErrorCode::Corruption);
  ExpectError(TryOpen(assemble([](BlockHandle data, std::vector<std::byte>& storage) {
                storage = HandleOf(data);
                storage.push_back(std::byte{0x00});
                return ByteView(storage);
              })),
              ErrorCode::Corruption);
  ExpectError(TryOpen(assemble([](BlockHandle data, std::vector<std::byte>& storage) {
                storage = HandleOf({.offset = data.offset, .size = 1000});
                return ByteView(storage);
              })),
              ErrorCode::Corruption);
  EXPECT_TRUE(TryOpen(assemble([](BlockHandle data, std::vector<std::byte>& storage) {
                storage = HandleOf(data);
                return ByteView(storage);
              })).has_value());
}

TEST_F(TableTest, RejectsIndexedBlocksThatOverlapOrGoBackwards) {
  const auto assemble = [&](auto choose) {
    TableAssembler table;
    const BlockHandle first = table.AddBlock(BlockOf({{Key("a", 1), AsBytes("x")}}));
    const BlockHandle second = table.AddBlock(BlockOf({{Key("b", 1), AsBytes("y")}}));
    const auto [low, high] = choose(first, second);
    const BlockHandle index =
        table.AddBlock(BlockOf({{Key("m", 1), HandleOf(low)}, {Key("z", 1), HandleOf(high)}}));
    return table.Finish(table.AddBlock(BlockOf({})), index);
  };

  EXPECT_TRUE(TryOpen(assemble([](BlockHandle first, BlockHandle second) {
                return std::pair(first, second);
              })).has_value());
  ExpectError(TryOpen(assemble(
                  [](BlockHandle first, BlockHandle second) { return std::pair(second, first); })),
              ErrorCode::Corruption);
  ExpectError(TryOpen(assemble([](BlockHandle first, BlockHandle) {
                return std::pair(BlockHandle{.offset = first.offset, .size = 0}, first);
              })),
              ErrorCode::Corruption);
}

TEST_F(TableTest, RejectsDamagedMetaindexAndFilterBlocksWhenFiltering) {
  std::vector<std::byte> corrupt_meta = FromHex(ThreeBlockTable);
  // The metaindex block starts at offset 115 in this table.
  corrupt_meta[120] ^= std::byte{0x01};
  ExpectError(TryOpen(corrupt_meta, WithFilter()), ErrorCode::Corruption);
  EXPECT_TRUE(TryOpen(corrupt_meta).has_value());

  const auto assemble = [&](ByteView filter_contents, bool trailing_byte) {
    TableAssembler table;
    const BlockHandle filter = table.AddBlock(filter_contents);
    std::vector<std::byte> handle = HandleOf(filter);
    if (trailing_byte) {
      handle.push_back(std::byte{0x00});
    }
    const BlockHandle meta =
        table.AddBlock(BlockOf({{AsBytes("filter.leveldb.BuiltinBloomFilter2"), handle}}));
    return table.Finish(meta, table.AddBlock(BlockOf({})));
  };
  const std::vector<std::byte> valid_filter = {std::byte{0}, std::byte{0}, std::byte{0},
                                               std::byte{0}, std::byte{11}};
  const std::vector<std::byte> invalid_filter = {std::byte{0}, std::byte{0}, std::byte{0},
                                                 std::byte{0}, std::byte{10}};

  EXPECT_TRUE(TryOpen(assemble(valid_filter, false), WithFilter()).has_value());
  ExpectError(TryOpen(assemble(valid_filter, true), WithFilter()), ErrorCode::Corruption);
  ExpectError(TryOpen(assemble(invalid_filter, false), WithFilter()), ErrorCode::Corruption);
  EXPECT_TRUE(TryOpen(assemble(invalid_filter, false)).has_value());

  TableAssembler outside;
  const BlockHandle meta = outside.AddBlock(BlockOf(
      {{AsBytes("filter.leveldb.BuiltinBloomFilter2"), HandleOf({.offset = 0, .size = 999})}}));
  ExpectError(TryOpen(outside.Finish(meta, outside.AddBlock(BlockOf({}))), WithFilter()),
              ErrorCode::Corruption);

  TableAssembler malformed;
  const std::vector<std::byte> not_a_handle = {std::byte{0x80}};
  const BlockHandle malformed_meta =
      malformed.AddBlock(BlockOf({{AsBytes("filter.leveldb.BuiltinBloomFilter2"), not_a_handle}}));
  ExpectError(
      TryOpen(malformed.Finish(malformed_meta, malformed.AddBlock(BlockOf({}))), WithFilter()),
      ErrorCode::Corruption);
}

TEST_F(TableTest, IgnoresFiltersOfOtherPolicies) {
  TableAssembler assembler;
  const BlockHandle data = assembler.AddBlock(BlockOf({{Key("a", 1), AsBytes("x")}}));
  const BlockHandle index = assembler.AddBlock(BlockOf({{Key("z", 1), HandleOf(data)}}));
  const BlockHandle meta = assembler.AddBlock(BlockOf({{AsBytes("filter.other"), HandleOf(data)}}));
  const auto table = Open(assembler.Finish(meta, index), WithFilter());
  ASSERT_NE(table, nullptr);

  ExpectValue(Get(*table, "a", 1), "x");
}

TEST_F(TableTest, CompletesShortReadsAndReportsTruncatedFiles) {
  const std::vector<std::byte> data = FromHex(ThreeBlockTable);
  state_ = std::make_shared<ReadableState>();
  state_->data = data;
  state_->maximum_chunk = 1;
  auto table =
      Table::Open(std::make_unique<MemoryRandomAccessFile>(state_), data.size(), comparator_, {});
  ASSERT_TRUE(table.has_value());
  ExpectValue(Get(**table, "apple", MaxSequenceNumber), "a");

  ExpectError(TryOpen(data, {}, data.size() + 10), ErrorCode::Corruption);
}

TEST_F(TableTest, ReportsReadFailuresWhileOpening) {
  for (const int failing_read : {1, 2, 3, 4}) {
    SCOPED_TRACE(failing_read);
    state_ = std::make_shared<ReadableState>();
    state_->data = FromHex(ThreeBlockTable);
    state_->fail_read_call = failing_read;
    ExpectError(Table::Open(std::make_unique<MemoryRandomAccessFile>(state_), state_->data.size(),
                            comparator_, WithFilter()),
                ErrorCode::Io);
  }

  state_ = std::make_shared<ReadableState>();
  state_->data = FromHex(ThreeBlockTable);
  state_->return_oversized_count = true;
  ExpectError(Table::Open(std::make_unique<MemoryRandomAccessFile>(state_), state_->data.size(),
                          comparator_, {}),
              ErrorCode::Io);
}

TEST_F(TableTest, ReportsDamagedDataBlocks) {
  std::vector<std::byte> corrupt = FromHex(ThreeBlockTable);
  corrupt[3] ^= std::byte{0x01};
  const auto table = Open(corrupt);
  ASSERT_NE(table, nullptr);

  ExpectError(TryGet(*table, "apple", MaxSequenceNumber), ErrorCode::Corruption);
  Table::Iterator iterator(*table);
  ExpectError(iterator.SeekToFirst(), ErrorCode::Corruption);
  EXPECT_FALSE(iterator.valid());
  ExpectValue(Get(*table, "apricot", MaxSequenceNumber), "b");

  TableAssembler empty_data;
  const BlockHandle data = empty_data.AddBlock(BlockOf({}));
  const BlockHandle index = empty_data.AddBlock(BlockOf({{Key("z", 1), HandleOf(data)}}));
  const auto empty = Open(empty_data.Finish(empty_data.AddBlock(BlockOf({})), index));
  ASSERT_NE(empty, nullptr);
  ExpectError(TryGet(*empty, "a", 1), ErrorCode::Corruption);
  Table::Iterator empty_iterator(*empty);
  ExpectError(empty_iterator.SeekToLast(), ErrorCode::Corruption);
}

TEST_F(TableTest, ReportsReadFailuresWhileIterating) {
  const auto table = Open(VersionedTable());
  ASSERT_NE(table, nullptr);
  const int reads = state_->read_calls;

  SCOPED_TRACE("forward");
  Table::Iterator forward(*table);
  ASSERT_TRUE(forward.SeekToFirst().has_value());
  state_->fail_read_call = reads + 2;
  ExpectError(forward.Next(), ErrorCode::Io);
  EXPECT_FALSE(forward.valid());

  SCOPED_TRACE("backward");
  Table::Iterator backward(*table);
  state_->fail_read_call.reset();
  ASSERT_TRUE(backward.SeekToLast().has_value());
  state_->fail_read_call = state_->read_calls + 1;
  ExpectError(backward.Prev(), ErrorCode::Io);
  EXPECT_FALSE(backward.valid());

  SCOPED_TRACE("seeking");
  Table::Iterator seeking(*table);
  state_->fail_read_call = state_->read_calls + 1;
  ExpectError(seeking.Seek(Key("k", 25)), ErrorCode::Io);
  state_->fail_read_call.reset();
  ASSERT_TRUE(seeking.Seek(Key("k", 29)).has_value());
  EXPECT_EQ(Materialize(seeking.key()), Key("k", 20, ValueKind::Deletion));

  SCOPED_TRACE("lookup");
  state_->fail_read_call = state_->read_calls + 1;
  ExpectError(TryGet(*table, "m", 5), ErrorCode::Io);

  SCOPED_TRACE("seeking into the next block");
  const auto shortened = Open(FromHex(ThreeBlockTable));
  ASSERT_NE(shortened, nullptr);
  Table::Iterator next_block(*shortened);
  // "apq" separates the first two blocks, so this seek reads both.
  state_->fail_read_call = state_->read_calls + 2;
  ExpectError(next_block.Seek(Key("apq", MaxSequenceNumber)), ErrorCode::Io);
  EXPECT_FALSE(next_block.valid());
}

TEST_F(TableTest, SupportsConcurrentLookups) {
  std::vector<Entry> entries;
  for (int index = 0; index < 200; ++index) {
    entries.push_back({Key("key" + std::to_string(1000 + index), 1),
                       Materialize(AsBytes("value" + std::to_string(index)))});
  }
  TableBuilderOptions builder_options;
  builder_options.block_size = 128;
  builder_options.filter_policy = BloomFilterPolicy(10);
  BlockCache cache(4096);
  TableOptions options = WithFilter();
  options.block_cache = &cache;
  const auto table = Open(BuildTable(entries, builder_options, comparator_), options);
  ASSERT_NE(table, nullptr);

  std::atomic<int> mismatches = 0;
  std::vector<std::thread> threads;
  for (int thread = 0; thread < 4; ++thread) {
    threads.emplace_back([&, thread] {
      for (int round = 0; round < 200; ++round) {
        const int index = (round * 7 + thread * 13) % 200;
        auto key = LookupKey::Create(AsBytes("key" + std::to_string(1000 + index)), 1);
        const auto lookup = table->Get(*key);
        if (!lookup.has_value() || !lookup->has_value() ||
            AsStringView((*lookup)->value) != "value" + std::to_string(index)) {
          ++mismatches;
        }
      }
    });
  }
  for (std::thread& thread : threads) {
    thread.join();
  }
  EXPECT_EQ(mismatches, 0);
}

TEST_F(TableTest, MatchesAnOrderedModel) {
  std::mt19937_64 random(20260924);
  const auto below = [&](std::size_t bound) {
    return std::uniform_int_distribution<std::size_t>(0, bound - 1)(random);
  };
  const auto user_key = [&] {
    std::string key;
    for (std::size_t length = below(6); length > 0; --length) {
      key.push_back("abc"[below(3)]);
    }
    return key;
  };
  for (int round = 0; round < 8; ++round) {
    SCOPED_TRACE(round);
    std::vector<Entry> entries;
    for (std::size_t count = below(200); count > 0; --count) {
      entries.push_back(
          {Key(user_key(), below(50), below(3) == 0 ? ValueKind::Deletion : ValueKind::Value),
           std::vector<std::byte>(below(12), std::byte{'v'})});
    }
    std::ranges::sort(entries, [&](const Entry& left, const Entry& right) {
      return comparator_.Compare(left.key, right.key) < 0;
    });
    const auto duplicates =
        std::ranges::unique(entries, [&](const Entry& left, const Entry& right) {
          return comparator_.Compare(left.key, right.key) == 0;
        });
    entries.erase(duplicates.begin(), duplicates.end());

    TableBuilderOptions builder_options;
    builder_options.block_size = std::array<std::size_t, 3>{1, 64, 4096}[below(3)];
    builder_options.restart_interval = below(2) == 0 ? 1 : 16;
    builder_options.filter_policy = BloomFilterPolicy(10);
    BlockCache cache(1024);
    TableOptions options = WithFilter();
    options.block_cache = below(2) == 0 ? &cache : nullptr;
    const auto table = Open(BuildTable(entries, builder_options, comparator_), options);
    ASSERT_NE(table, nullptr);

    EXPECT_EQ(ScanForward(*table), entries);
    EXPECT_EQ(ScanBackward(*table), entries);

    Table::Iterator iterator(*table);
    for (int seek = 0; seek < 30; ++seek) {
      const std::string user = user_key();
      const SequenceNumber sequence = below(60);
      const std::vector<std::byte> target = Key(user, sequence);
      const auto lower = std::ranges::lower_bound(
          entries, ByteView(target),
          [&](ByteView left, ByteView right) { return comparator_.Compare(left, right) < 0; },
          [](const Entry& entry) { return ByteView(entry.key); });
      const auto first = static_cast<std::size_t>(lower - entries.begin());
      std::size_t index = first;
      ASSERT_TRUE(iterator.Seek(target).has_value());
      for (int step = 0; step < 4; ++step) {
        ASSERT_EQ(iterator.valid(), index < entries.size());
        if (!iterator.valid()) {
          break;
        }
        EXPECT_EQ(Materialize(iterator.key()), entries[index].key);
        if (below(2) == 0) {
          ASSERT_TRUE(iterator.Next().has_value());
          ++index;
        } else if (index > 0) {
          ASSERT_TRUE(iterator.Prev().has_value());
          --index;
        }
      }

      // The lookup finds the first entry at or after the target if it has the
      // target's user key.
      const auto lookup = Get(*table, user, sequence);
      const auto parsed = first < entries.size()
                              ? ParseInternalKey(entries[first].key)
                              : Result<ParsedInternalKey>(std::unexpected(Error::NotFound("")));
      if (parsed.has_value() && AsStringView(parsed->user_key) == user) {
        ASSERT_TRUE(lookup.has_value()) << user << "@" << sequence;
        EXPECT_EQ(lookup->kind, parsed->kind);
        EXPECT_EQ(lookup->value, entries[first].value);
      } else {
        EXPECT_FALSE(lookup.has_value()) << user << "@" << sequence;
      }
    }
  }
}

}  // namespace
}  // namespace modern_leveldb
