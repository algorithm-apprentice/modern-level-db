#include "engine/table_cache.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "format/internal_key.h"
#include "metadata/filenames.h"
#include "modern_leveldb/base/bytes.h"
#include "modern_leveldb/base/comparator.h"
#include "modern_leveldb/base/result.h"
#include "platform/file_system.h"
#include "table/bloom_filter.h"
#include "table/table.h"
#include "table/table_builder.h"

namespace modern_leveldb {
namespace {

static_assert(!std::is_copy_constructible_v<TableCache>);
static_assert(!std::is_move_constructible_v<TableCache>);
static_assert(
    std::is_constructible_v<TableCache, FileSystem&, std::filesystem::path,
                            const InternalKeyComparator&, const TableOptions&, std::size_t>);
static_assert(!std::is_constructible_v<TableCache, FileSystem&, std::filesystem::path,
                                       InternalKeyComparator, const TableOptions&, std::size_t>);

class MemoryRandomAccessFile final : public RandomAccessFile {
 public:
  MemoryRandomAccessFile(std::shared_ptr<const std::vector<std::byte>> data,
                         std::shared_ptr<std::atomic<int>> reads)
      : data_(std::move(data)), reads_(std::move(reads)) {}

  Result<std::size_t> Read(std::uint64_t offset, MutableByteView output) const override {
    ++*reads_;
    if (offset >= data_->size()) {
      return 0;
    }
    const std::size_t count =
        std::min(output.size(), data_->size() - static_cast<std::size_t>(offset));
    std::copy_n(data_->begin() + static_cast<std::ptrdiff_t>(offset), count, output.begin());
    return count;
  }

 private:
  std::shared_ptr<const std::vector<std::byte>> data_;
  std::shared_ptr<std::atomic<int>> reads_;
};

// Serves random-access files from memory. An open file reads the contents the
// path had when it was opened.
class MemoryFileSystem final : public FileSystem {
 public:
  void Write(const std::filesystem::path& path, std::vector<std::byte> contents) {
    std::lock_guard lock(mutex_);
    files_[path] = std::make_shared<const std::vector<std::byte>>(std::move(contents));
  }

  void FailOpens(std::optional<Error> error) {
    std::lock_guard lock(mutex_);
    open_error_ = std::move(error);
  }

  [[nodiscard]] int opens(const std::filesystem::path& path) const {
    std::lock_guard lock(mutex_);
    const auto found = opens_.find(path);
    return found == opens_.end() ? 0 : found->second;
  }

  [[nodiscard]] int reads() const { return reads_->load(); }

  Result<std::unique_ptr<RandomAccessFile>> OpenRandomAccess(
      const std::filesystem::path& path) override {
    std::lock_guard lock(mutex_);
    ++opens_[path];
    if (open_error_.has_value()) {
      return std::unexpected(*open_error_);
    }
    const auto found = files_.find(path);
    if (found == files_.end()) {
      return std::unexpected(Error::NotFound(path.string()));
    }
    return std::make_unique<MemoryRandomAccessFile>(found->second, reads_);
  }

  Result<std::unique_ptr<SequentialFile>> OpenSequential(const std::filesystem::path&) override {
    return Unused();
  }
  Result<std::unique_ptr<WritableFile>> OpenWritable(const std::filesystem::path&) override {
    return Unused();
  }
  Result<std::unique_ptr<WritableFile>> OpenAppendable(const std::filesystem::path&) override {
    return Unused();
  }
  Result<bool> FileExists(const std::filesystem::path&) const override { return Unused(); }
  Result<std::vector<std::filesystem::path>> ListDirectory(
      const std::filesystem::path&) const override {
    return Unused();
  }
  Result<std::uint64_t> FileSize(const std::filesystem::path&) const override { return Unused(); }
  Status CreateDirectory(const std::filesystem::path&) override { return Unused(); }
  Status RemoveFile(const std::filesystem::path&) override { return Unused(); }
  Status RemoveDirectory(const std::filesystem::path&) override { return Unused(); }
  Status RenameFile(const std::filesystem::path&, const std::filesystem::path&) override {
    return Unused();
  }
  Status SyncDirectory(const std::filesystem::path&) override { return Unused(); }
  Result<std::unique_ptr<FileLock>> LockFile(const std::filesystem::path&) override {
    return Unused();
  }

 private:
  static std::unexpected<Error> Unused() {
    return std::unexpected(Error::NotSupported("not used by the table cache"));
  }

  mutable std::mutex mutex_;
  std::map<std::filesystem::path, std::shared_ptr<const std::vector<std::byte>>> files_;
  std::map<std::filesystem::path, int> opens_;
  std::optional<Error> open_error_;
  std::shared_ptr<std::atomic<int>> reads_ = std::make_shared<std::atomic<int>>(0);
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

// Returns the value of the user key at the newest sequence number, if any,
// without test assertions, so that any thread may call it.
std::optional<std::string> TryLookup(const TableCache::Handle& table, std::string_view user_key) {
  auto key = LookupKey::Create(AsBytes(user_key), 100);
  if (!key.has_value()) {
    return std::nullopt;
  }
  auto lookup = table->Get(*key);
  if (!lookup.has_value() || !lookup->has_value()) {
    return std::nullopt;
  }
  return std::string(AsStringView((*lookup)->value));
}

class TableCacheTest : public testing::Test {
 protected:
  // Writes a table whose only entry maps "key" to the value as the file of the
  // number, and returns the table's size.
  std::uint64_t WriteTable(std::uint64_t number, std::string_view value,
                           const TableBuilderOptions& options = {}) {
    std::vector<std::byte> data;
    TableBuilder builder(std::make_unique<MemoryWritableFile>(data), comparator_, options);
    auto key = InternalKey::Create(AsBytes("key"), 1, ValueKind::Value);
    EXPECT_TRUE(key.has_value());
    EXPECT_TRUE(builder.Add(key->encoded(), AsBytes(value)).has_value());
    EXPECT_TRUE(builder.Finish().has_value());
    const std::uint64_t size = data.size();
    file_system_.Write(TableFileName(directory_, number), std::move(data));
    return size;
  }

  [[nodiscard]] int Opens(std::uint64_t number) const {
    return file_system_.opens(TableFileName(directory_, number));
  }

  static TableCache::Handle Found(Result<TableCache::Handle> table) {
    EXPECT_TRUE(table.has_value()) << table.error().ToString();
    return std::move(table).value();
  }

  static void ExpectError(const Result<TableCache::Handle>& table, ErrorCode code) {
    ASSERT_FALSE(table.has_value());
    EXPECT_EQ(table.error().code(), code) << table.error().ToString();
  }

  const std::filesystem::path directory_ = std::filesystem::path("db");
  MemoryFileSystem file_system_;
  InternalKeyComparator comparator_{BytewiseComparator()};
};

TEST_F(TableCacheTest, OpensTheTableFileOfTheNumber) {
  const std::uint64_t size = WriteTable(7, "seven");
  TableCache cache(file_system_, directory_, comparator_, {}, 10);

  const TableCache::Handle table = Found(cache.Find(7, size));

  EXPECT_EQ(file_system_.opens(directory_ / "000007.ldb"), 1);
  EXPECT_EQ(TryLookup(table, "key"), "seven");
  Table::Iterator iterator(*table);
  ASSERT_TRUE(iterator.SeekToFirst().has_value());
  ASSERT_TRUE(iterator.valid());
  EXPECT_EQ(AsStringView(iterator.value()), "seven");
}

TEST_F(TableCacheTest, ReusesCachedTablesUntilEvicted) {
  const std::uint64_t seven = WriteTable(7, "seven");
  const std::uint64_t eight = WriteTable(8, "eight");
  TableCache cache(file_system_, directory_, comparator_, {}, 10);

  const TableCache::Handle first = Found(cache.Find(7, seven));
  // A cached table ignores the size.
  const TableCache::Handle again = Found(cache.Find(7, seven + 1));
  const TableCache::Handle other = Found(cache.Find(8, eight));
  EXPECT_EQ(Opens(7), 1);
  EXPECT_EQ(Opens(8), 1);
  EXPECT_EQ(TryLookup(again, "key"), "seven");
  EXPECT_EQ(TryLookup(other, "key"), "eight");

  cache.Evict(7);
  cache.Evict(9);
  EXPECT_EQ(TryLookup(first, "key"), "seven");
  const TableCache::Handle reopened = Found(cache.Find(7, seven));
  static_cast<void>(Found(cache.Find(8, eight)));
  EXPECT_EQ(Opens(7), 2);
  EXPECT_EQ(Opens(8), 1);
  EXPECT_EQ(TryLookup(reopened, "key"), "seven");
}

TEST_F(TableCacheTest, OpensTablesWithTheTableOptions) {
  TableBuilderOptions filtered;
  filtered.filter_policy = BloomFilterPolicy(10);
  const std::uint64_t size = WriteTable(7, "seven", filtered);
  BlockCache blocks(1 << 20);
  TableOptions options;
  options.filter_policy = BloomFilterPolicy(10);
  options.block_cache = &blocks;
  TableCache cache(file_system_, directory_, comparator_, options, 10);
  const TableCache::Handle table = Found(cache.Find(7, size));

  // "a" precedes the index key, so only the filter can rule it out without
  // reading the data block.
  const int reads = file_system_.reads();
  EXPECT_EQ(TryLookup(table, "a"), std::nullopt);
  EXPECT_EQ(file_system_.reads(), reads);
  EXPECT_EQ(TryLookup(table, "key"), "seven");
  EXPECT_GT(blocks.total_charge(), 0U);
}

TEST_F(TableCacheTest, KeepsARoundedCapacityOfUnpinnedTables) {
  constexpr std::uint64_t Tables = 64;
  std::vector<std::uint64_t> sizes(Tables + 1);
  for (std::uint64_t number = 1; number <= Tables; ++number) {
    sizes[number] = WriteTable(number, "value" + std::to_string(number));
  }
  TableCache cache(file_system_, directory_, comparator_, {}, 1);
  const TableCache::Handle pinned = Found(cache.Find(1, sizes[1]));
  for (std::uint64_t number = 2; number <= Tables; ++number) {
    static_cast<void>(Found(cache.Find(number, sizes[number])));
  }

  // Every insertion leaves its shard at most one table that no handle holds,
  // besides pinned table 1, and each table is found once more below, so at
  // most 16 of tables 2 to 64 are still cached: at least 47 are reopened.
  int reopened = 0;
  for (std::uint64_t number = 1; number <= Tables; ++number) {
    const int before = Opens(number);
    static_cast<void>(Found(cache.Find(number, sizes[number])));
    reopened += Opens(number) - before;
  }
  EXPECT_GE(reopened, 47);
  EXPECT_EQ(Opens(1), 1);
  EXPECT_EQ(TryLookup(pinned, "key"), "value1");
}

TEST_F(TableCacheTest, CachesNothingWithoutCapacity) {
  const std::uint64_t size = WriteTable(7, "seven");
  TableCache cache(file_system_, directory_, comparator_, {}, 0);

  const TableCache::Handle first = Found(cache.Find(7, size));
  const TableCache::Handle second = Found(cache.Find(7, size));

  EXPECT_EQ(Opens(7), 2);
  EXPECT_EQ(TryLookup(first, "key"), "seven");
  EXPECT_EQ(TryLookup(second, "key"), "seven");
}

TEST_F(TableCacheTest, RetriesFailedOpens) {
  TableCache cache(file_system_, directory_, comparator_, {}, 10);

  ExpectError(cache.Find(7, 100), ErrorCode::NotFound);
  const std::uint64_t seven = WriteTable(7, "seven");
  EXPECT_EQ(TryLookup(Found(cache.Find(7, seven)), "key"), "seven");
  EXPECT_EQ(Opens(7), 2);

  const std::uint64_t eight = WriteTable(8, "eight");
  file_system_.FailOpens(Error::Io("injected open failure"));
  ExpectError(cache.Find(8, eight), ErrorCode::Io);
  file_system_.FailOpens(std::nullopt);
  EXPECT_EQ(TryLookup(Found(cache.Find(8, eight)), "key"), "eight");
  EXPECT_EQ(Opens(8), 2);

  file_system_.Write(TableFileName(directory_, 9), std::vector<std::byte>(100));
  ExpectError(cache.Find(9, 100), ErrorCode::Corruption);
  const std::uint64_t nine = WriteTable(9, "nine");
  EXPECT_EQ(TryLookup(Found(cache.Find(9, nine)), "key"), "nine");
  EXPECT_EQ(Opens(9), 2);
}

TEST_F(TableCacheTest, SupportsConcurrentFindsAndEvictions) {
  constexpr std::uint64_t Tables = 8;
  std::vector<std::uint64_t> sizes(Tables + 1);
  for (std::uint64_t number = 1; number <= Tables; ++number) {
    sizes[number] = WriteTable(number, "value" + std::to_string(number));
  }
  TableCache cache(file_system_, directory_, comparator_, {}, 4);
  std::atomic<int> failures = 0;

  std::vector<std::thread> threads;
  for (std::uint64_t thread = 0; thread < 4; ++thread) {
    threads.emplace_back([&, thread] {
      for (std::uint64_t step = 0; step < 300; ++step) {
        const std::uint64_t number = (step * 7 + thread) % Tables + 1;
        if (step % 5 == 0) {
          cache.Evict(number);
          continue;
        }
        Result<TableCache::Handle> table = cache.Find(number, sizes[number]);
        if (!table.has_value() || TryLookup(*table, "key") != "value" + std::to_string(number)) {
          ++failures;
        }
      }
    });
  }
  for (std::thread& thread : threads) {
    thread.join();
  }

  EXPECT_EQ(failures.load(), 0);
}

}  // namespace
}  // namespace modern_leveldb
