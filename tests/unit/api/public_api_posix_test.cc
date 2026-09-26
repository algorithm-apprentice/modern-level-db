#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "modern_leveldb/base/bytes.h"
#include "modern_leveldb/base/comparator.h"
#include "modern_leveldb/base/result.h"
#include "modern_leveldb/db.h"

namespace modern_leveldb {
namespace {

class TemporaryDatabaseDirectory {
 public:
  TemporaryDatabaseDirectory()
      : path_(std::filesystem::temp_directory_path() /
              ("modern-leveldb-public-api-" +
               std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()))) {}

  TemporaryDatabaseDirectory(const TemporaryDatabaseDirectory&) = delete;
  TemporaryDatabaseDirectory& operator=(const TemporaryDatabaseDirectory&) = delete;

  ~TemporaryDatabaseDirectory() {
    std::error_code error;
    std::filesystem::remove_all(path_, error);
  }

  [[nodiscard]] const std::filesystem::path& path() const noexcept { return path_; }

 private:
  std::filesystem::path path_;
};

Options CreatingOptions() {
  Options options;
  options.create_if_missing = true;
  return options;
}

std::string Text(ByteView value) { return std::string(AsStringView(value)); }

template <typename T>
void ExpectInvalid(Result<T> result) {
  ASSERT_FALSE(result.has_value());
  EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
}

class TrackingComparator final : public Comparator {
 public:
  explicit TrackingComparator(std::shared_ptr<std::atomic<bool>> destroyed)
      : destroyed_(std::move(destroyed)) {}

  ~TrackingComparator() override { destroyed_->store(true); }

  [[nodiscard]] int Compare(ByteView left, ByteView right) const noexcept override {
    return BytewiseComparator().Compare(left, right);
  }

  [[nodiscard]] std::string_view Name() const noexcept override {
    return "modern-leveldb.test.TrackingComparator";
  }

  void FindShortestSeparator(std::vector<std::byte>& start, ByteView limit) const override {
    BytewiseComparator().FindShortestSeparator(start, limit);
  }

  void FindShortSuccessor(std::vector<std::byte>& key) const override {
    BytewiseComparator().FindShortSuccessor(key);
  }

 private:
  std::shared_ptr<std::atomic<bool>> destroyed_;
};

TEST(PublicDatabaseTest, WritesReadsDeletesBatchesAndReopens) {
  TemporaryDatabaseDirectory directory;
  {
    Result<Database> opened = Database::Open(CreatingOptions(), directory.path());
    ASSERT_TRUE(opened.has_value()) << opened.error().ToString();
    Database database = std::move(*opened);

    ASSERT_TRUE(database.Put(AsBytes("a"), AsBytes("1")).has_value());
    WriteBatch batch;
    ASSERT_TRUE(batch.Put(AsBytes("b"), AsBytes("2")).has_value());
    ASSERT_TRUE(batch.Delete(AsBytes("a")).has_value());
    WriteOptions write_options{.sync = true};
    ASSERT_TRUE(database.Write(batch, write_options).has_value());

    const auto deleted = database.Get(AsBytes("a"));
    ASSERT_TRUE(deleted.has_value());
    EXPECT_FALSE(deleted->has_value());
    const auto value = database.Get(AsBytes("b"));
    ASSERT_TRUE(value.has_value() && value->has_value());
    EXPECT_EQ(Text(**value), "2");
    ASSERT_TRUE(database.Delete(AsBytes("missing")).has_value());
  }

  Result<Database> reopened = Database::Open(Options(), directory.path());
  ASSERT_TRUE(reopened.has_value()) << reopened.error().ToString();
  const auto value = reopened->Get(AsBytes("b"));
  ASSERT_TRUE(value.has_value() && value->has_value());
  EXPECT_EQ(Text(**value), "2");
}

TEST(PublicDatabaseTest, IteratesAndSeeksInBothDirections) {
  TemporaryDatabaseDirectory directory;
  Result<Database> opened = Database::Open(CreatingOptions(), directory.path());
  ASSERT_TRUE(opened.has_value()) << opened.error().ToString();
  Database database = std::move(*opened);
  ASSERT_TRUE(database.Put(AsBytes("a"), AsBytes("1")).has_value());
  ASSERT_TRUE(database.Put(AsBytes("c"), AsBytes("3")).has_value());
  ASSERT_TRUE(database.Put(AsBytes("b"), AsBytes("2")).has_value());

  Result<Iterator> created = database.NewIterator();
  ASSERT_TRUE(created.has_value()) << created.error().ToString();
  Iterator iterator = std::move(*created);
  EXPECT_FALSE(iterator.valid());
  ASSERT_TRUE(iterator.SeekToFirst().has_value());
  ASSERT_TRUE(iterator.valid());
  EXPECT_EQ(Text(iterator.key()), "a");
  EXPECT_EQ(Text(iterator.value()), "1");
  ASSERT_TRUE(iterator.Next().has_value());
  EXPECT_EQ(Text(iterator.key()), "b");
  ASSERT_TRUE(iterator.Seek(AsBytes("bb")).has_value());
  EXPECT_EQ(Text(iterator.key()), "c");
  ASSERT_TRUE(iterator.SeekToLast().has_value());
  EXPECT_EQ(Text(iterator.key()), "c");
  ASSERT_TRUE(iterator.Prev().has_value());
  EXPECT_EQ(Text(iterator.key()), "b");
}

TEST(PublicDatabaseTest, IteratorRetainsItsSnapshotAfterTheSnapshotHandleIsDestroyed) {
  TemporaryDatabaseDirectory directory;
  Result<Database> opened = Database::Open(CreatingOptions(), directory.path());
  ASSERT_TRUE(opened.has_value()) << opened.error().ToString();
  Database database = std::move(*opened);
  ASSERT_TRUE(database.Put(AsBytes("a"), AsBytes("old")).has_value());

  Result<Iterator> created = [&] {
    Result<Snapshot> snapshot = database.GetSnapshot();
    EXPECT_TRUE(snapshot.has_value());
    if (!snapshot.has_value()) {
      return Result<Iterator>(std::unexpected(snapshot.error()));
    }
    ReadOptions options{.snapshot = &*snapshot};
    return database.NewIterator(options);
  }();
  ASSERT_TRUE(created.has_value()) << created.error().ToString();
  ASSERT_TRUE(database.Put(AsBytes("a"), AsBytes("new")).has_value());

  Iterator iterator = std::move(*created);
  ASSERT_TRUE(iterator.SeekToFirst().has_value());
  ASSERT_TRUE(iterator.valid());
  EXPECT_EQ(Text(iterator.key()), "a");
  EXPECT_EQ(Text(iterator.value()), "old");
}

TEST(PublicDatabaseTest, RejectsForeignAndMovedFromHandles) {
  TemporaryDatabaseDirectory first_directory;
  TemporaryDatabaseDirectory second_directory;
  Result<Database> first_opened = Database::Open(CreatingOptions(), first_directory.path());
  Result<Database> second_opened = Database::Open(CreatingOptions(), second_directory.path());
  ASSERT_TRUE(first_opened.has_value()) << first_opened.error().ToString();
  ASSERT_TRUE(second_opened.has_value()) << second_opened.error().ToString();
  Database first = std::move(*first_opened);
  Database second = std::move(*second_opened);

  Result<Snapshot> snapshot = first.GetSnapshot();
  ASSERT_TRUE(snapshot.has_value()) << snapshot.error().ToString();
  ReadOptions foreign{.snapshot = &*snapshot};
  const auto foreign_read = second.Get(AsBytes("a"), foreign);
  ASSERT_FALSE(foreign_read.has_value());
  EXPECT_EQ(foreign_read.error().code(), ErrorCode::InvalidArgument);
  const auto foreign_iterator = second.NewIterator(foreign);
  ASSERT_FALSE(foreign_iterator.has_value());
  EXPECT_EQ(foreign_iterator.error().code(), ErrorCode::InvalidArgument);

  Snapshot retained = std::move(*snapshot);
  ReadOptions moved_snapshot{.snapshot = &*snapshot};
  const auto moved_read = first.Get(AsBytes("a"), moved_snapshot);
  ASSERT_FALSE(moved_read.has_value());
  EXPECT_EQ(moved_read.error().code(), ErrorCode::InvalidArgument);

  Database moved = std::move(first);
  ExpectInvalid(first.Put(AsBytes("a"), AsBytes("1")));
  ExpectInvalid(first.Delete(AsBytes("a")));
  WriteBatch valid_batch;
  ExpectInvalid(first.Write(valid_batch));
  ExpectInvalid(first.Get(AsBytes("a")));
  ExpectInvalid(first.NewIterator());
  ExpectInvalid(first.GetSnapshot());

  Result<Iterator> iterator = moved.NewIterator();
  ASSERT_TRUE(iterator.has_value()) << iterator.error().ToString();
  Iterator retained_iterator = std::move(*iterator);
  EXPECT_FALSE(iterator->valid());
  ExpectInvalid(iterator->SeekToFirst());
  ExpectInvalid(iterator->SeekToLast());
  ExpectInvalid(iterator->Seek(AsBytes("a")));
  ExpectInvalid(iterator->Next());
  ExpectInvalid(iterator->Prev());

  Result<Iterator> assigned_source = moved.NewIterator();
  Result<Iterator> assigned_target = moved.NewIterator();
  ASSERT_TRUE(assigned_source.has_value() && assigned_target.has_value());
  *assigned_target = std::move(*assigned_source);
  EXPECT_FALSE(assigned_source->valid());

  Result<Snapshot> snapshot_source = moved.GetSnapshot();
  Result<Snapshot> snapshot_target = moved.GetSnapshot();
  ASSERT_TRUE(snapshot_source.has_value() && snapshot_target.has_value());
  *snapshot_target = std::move(*snapshot_source);

  Database assigned = std::move(second);
  assigned = std::move(moved);

  WriteBatch moved_batch_source;
  WriteBatch moved_batch = std::move(moved_batch_source);
  ExpectInvalid(assigned.Write(moved_batch_source));
  EXPECT_TRUE(assigned.Write(moved_batch).has_value());
}

TEST(PublicDatabaseTest, ChildHandlesKeepTheEngineAlive) {
  TemporaryDatabaseDirectory iterator_directory;
  std::optional<Iterator> iterator;
  {
    Result<Database> opened = Database::Open(CreatingOptions(), iterator_directory.path());
    ASSERT_TRUE(opened.has_value()) << opened.error().ToString();
    ASSERT_TRUE(opened->Put(AsBytes("a"), AsBytes("1")).has_value());
    Result<Iterator> created = opened->NewIterator();
    ASSERT_TRUE(created.has_value()) << created.error().ToString();
    iterator.emplace(std::move(*created));
  }
  const Result<Database> iterator_locked = Database::Open(Options(), iterator_directory.path());
  ASSERT_FALSE(iterator_locked.has_value());
  EXPECT_EQ(iterator_locked.error().code(), ErrorCode::Busy);
  ASSERT_TRUE(iterator->SeekToFirst().has_value());
  ASSERT_TRUE(iterator->valid());
  EXPECT_EQ(Text(iterator->value()), "1");
  iterator.reset();
  EXPECT_TRUE(Database::Open(Options(), iterator_directory.path()).has_value());

  TemporaryDatabaseDirectory snapshot_directory;
  std::optional<Snapshot> snapshot;
  {
    Result<Database> opened = Database::Open(CreatingOptions(), snapshot_directory.path());
    ASSERT_TRUE(opened.has_value()) << opened.error().ToString();
    Result<Snapshot> created = opened->GetSnapshot();
    ASSERT_TRUE(created.has_value()) << created.error().ToString();
    snapshot.emplace(std::move(*created));
  }
  const Result<Database> snapshot_locked = Database::Open(Options(), snapshot_directory.path());
  ASSERT_FALSE(snapshot_locked.has_value());
  EXPECT_EQ(snapshot_locked.error().code(), ErrorCode::Busy);
  snapshot.reset();
  EXPECT_TRUE(Database::Open(Options(), snapshot_directory.path()).has_value());
}

TEST(PublicDatabaseTest, ValidatesOptionsAndRetainsTheComparator) {
  TemporaryDatabaseDirectory invalid_directory;
  Options invalid = CreatingOptions();
  invalid.block_restart_interval = 0;
  const Result<Database> rejected = Database::Open(invalid, invalid_directory.path());
  ASSERT_FALSE(rejected.has_value());
  EXPECT_EQ(rejected.error().code(), ErrorCode::InvalidArgument);
  EXPECT_FALSE(std::filesystem::exists(invalid_directory.path()));

  for (const Compression compression :
       {static_cast<Compression>(-1), static_cast<Compression>(3)}) {
    SCOPED_TRACE(static_cast<int>(compression));
    TemporaryDatabaseDirectory directory;
    Options invalid_compression = CreatingOptions();
    invalid_compression.compression = compression;
    const Result<Database> result = Database::Open(invalid_compression, directory.path());
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
    EXPECT_FALSE(std::filesystem::exists(directory.path()));
  }

  for (const int level : {-6, 23}) {
    SCOPED_TRACE(level);
    TemporaryDatabaseDirectory directory;
    Options invalid_level = CreatingOptions();
    invalid_level.compression = Compression::Zstd;
    invalid_level.zstd_compression_level = level;
    const Result<Database> result = Database::Open(invalid_level, directory.path());
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code(), ErrorCode::InvalidArgument);
    EXPECT_FALSE(std::filesystem::exists(directory.path()));
  }

  TemporaryDatabaseDirectory comparator_directory;
  auto destroyed = std::make_shared<std::atomic<bool>>(false);
  auto comparator = std::make_shared<TrackingComparator>(destroyed);
  std::weak_ptr<const Comparator> retained = comparator;
  Options options = CreatingOptions();
  options.comparator = comparator;
  Result<Database> opened = Database::Open(std::move(options), comparator_directory.path());
  ASSERT_TRUE(opened.has_value()) << opened.error().ToString();
  comparator.reset();
  EXPECT_FALSE(retained.expired());
  ASSERT_TRUE(opened->Put(AsBytes("a"), AsBytes("1")).has_value());
  opened = std::unexpected(Error::Aborted("release the database"));
  EXPECT_TRUE(retained.expired());
  EXPECT_TRUE(destroyed->load());
}

TEST(PublicDatabaseTest, WiresBloomFiltersIntoWrittenTables) {
  TemporaryDatabaseDirectory directory;
  Options options = CreatingOptions();
  options.write_buffer_size = 1;
  options.block_restart_interval = 4;
  options.bloom_bits_per_key = 10;
  {
    Result<Database> opened = Database::Open(options, directory.path());
    ASSERT_TRUE(opened.has_value()) << opened.error().ToString();
    const std::string large(std::size_t{70} << 10U, 'v');
    ASSERT_TRUE(opened->Put(AsBytes("large"), AsBytes(large)).has_value());
    ASSERT_TRUE(opened->Put(AsBytes("trigger"), AsBytes("1")).has_value());
  }

  options.create_if_missing = false;
  Result<Database> reopened = Database::Open(options, directory.path());
  ASSERT_TRUE(reopened.has_value()) << reopened.error().ToString();
  const auto value = reopened->Get(AsBytes("large"));
  ASSERT_TRUE(value.has_value() && value->has_value());
  EXPECT_EQ(value->value().size(), std::size_t{70} << 10U);
}

TEST(PublicDatabaseTest, WritesAndReopensEveryCompressionMode) {
  for (const Compression compression :
       {Compression::None, Compression::Snappy, Compression::Zstd}) {
    SCOPED_TRACE(static_cast<int>(compression));
    TemporaryDatabaseDirectory directory;
    Options options = CreatingOptions();
    options.write_buffer_size = 1;
    options.compression = compression;
    if (compression == Compression::Zstd) {
      options.zstd_compression_level = -5;
    }
    {
      Result<Database> opened = Database::Open(options, directory.path());
      ASSERT_TRUE(opened.has_value()) << opened.error().ToString();
      const std::string large(std::size_t{70} << 10U, 'v');
      ASSERT_TRUE(opened->Put(AsBytes("large"), AsBytes(large)).has_value());
      ASSERT_TRUE(opened->Put(AsBytes("trigger"), AsBytes("1")).has_value());
    }

    Result<Database> reopened = Database::Open(Options(), directory.path());
    ASSERT_TRUE(reopened.has_value()) << reopened.error().ToString();
    const auto value = reopened->Get(AsBytes("large"));
    ASSERT_TRUE(value.has_value() && value->has_value());
    EXPECT_EQ(value->value().size(), std::size_t{70} << 10U);
  }
}

}  // namespace
}  // namespace modern_leveldb
