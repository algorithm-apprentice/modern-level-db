#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "dependency_test_support.h"
#include "modern_leveldb/base/bytes.h"
#include "modern_leveldb/base/comparator.h"
#include "modern_leveldb/base/result.h"
#include "modern_leveldb/block_cache.h"
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

Options OpeningOptions() {
    Options options;
#if defined(_WIN32)
    options.allow_weak_namespace_durability = true;
#endif
    return options;
}

Options CreatingOptions() {
    Options options = OpeningOptions();
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

struct FilterInspection {
    std::size_t tables = 0;
    std::vector<std::string> names;
};

// The fixtures disable compression and contain only numeric/filler keys and
// repeated value bytes; the two policy names are unambiguous wire markers.
Result<FilterInspection> InspectFilters(const std::filesystem::path& directory) {
    FilterInspection result;
    for (const auto& path : std::filesystem::directory_iterator(directory)) {
        if (path.path().extension() != ".ldb") {
            continue;
        }
        ++result.tables;
        std::ifstream input(path.path(), std::ios::binary | std::ios::ate);
        if (!input.is_open()) {
            return std::unexpected(Error::Io("cannot inspect generated table"));
        }
        const std::streamoff length = input.tellg();
        if (!std::in_range<std::size_t>(length) || !std::in_range<std::streamsize>(length)) {
            return std::unexpected(Error::Io("cannot inspect generated table size"));
        }
        std::string stored(static_cast<std::size_t>(length), '\0');
        input.seekg(0);
        if (!input.read(stored.data(), static_cast<std::streamsize>(stored.size()))) {
            return std::unexpected(Error::Io("cannot read generated table"));
        }
        for (const auto name :
             {"filter.modern_leveldb.FullBloom1", "filter.leveldb.BuiltinBloomFilter2"}) {
            if (stored.find(name) != std::string::npos) {
                result.names.emplace_back(name);
            }
        }
    }
    return result;
}

class AutomaticFilterComparator final : public Comparator {
public:
    int Compare(ByteView left, ByteView right) const noexcept override {
        const auto common = std::min(left.size(), right.size());
        for (std::size_t index = 0; index < common; ++index) {
            const auto fold = [](std::byte value) {
                const auto byte = std::to_integer<unsigned>(value);
                return byte >= 'A' && byte <= 'Z' ? byte + ('a' - 'A') : byte;
            };
            if (fold(left[index]) != fold(right[index])) {
                return fold(left[index]) < fold(right[index]) ? -1 : 1;
            }
        }
        return left.size() == right.size() ? 0 : (left.size() < right.size() ? -1 : 1);
    }
    std::string_view Name() const noexcept override { return BytewiseComparator().Name(); }
    void FindShortestSeparator(std::vector<std::byte>&, ByteView) const override {}
    void FindShortSuccessor(std::vector<std::byte>&) const override {}
};

TEST(PublicFilterTest, AutomaticDefaultsExplicitLegacyAndDisableWriteTheSelectedMetadata) {
    for (const auto mode : {0, 1, 2, 3, 4}) {
        TemporaryDatabaseDirectory directory;
        auto options = CreatingOptions();
        options.reuse_table_readers = mode == 1 || mode == 3;
        options.write_buffer_size = 64 * 1024;
        options.compression = Compression::None;
        if (mode == 1) {
            options.bloom_filter_layout = BloomFilterLayout::BlockBased;
        } else if (mode == 2) {
            options.bloom_bits_per_key.reset();
        } else if (mode == 3) {
            options.comparator =
                std::shared_ptr<const Comparator>(&BytewiseComparator(), [](const Comparator*) {});
        } else if (mode == 4) {
            options.comparator =
                std::make_shared<TrackingComparator>(std::make_shared<std::atomic<bool>>(false));
            options.bloom_filter_layout = BloomFilterLayout::FullTable;
        }
        {
            auto opened = Database::Open(options, directory.path());
            ASSERT_TRUE(opened.has_value());
            for (unsigned index = 0; index < 256; ++index) {
                const auto key = "key-" + std::to_string(index);
                const std::string value(1024, 'x');
                ASSERT_TRUE(opened->Put(AsBytes(key), AsBytes(value)).has_value());
            }
        }
        auto inspected = InspectFilters(directory.path());
        ASSERT_TRUE(inspected.has_value());
        ASSERT_GT(inspected->tables, 0U);
        if (mode == 2) {
            EXPECT_TRUE(inspected->names.empty());
        } else {
            ASSERT_FALSE(inspected->names.empty());
            EXPECT_EQ(inspected->names.size(), inspected->tables);
            for (const auto& name : inspected->names) {
                EXPECT_EQ(name, mode == 1 ? "filter.leveldb.BuiltinBloomFilter2"
                                          : "filter.modern_leveldb.FullBloom1");
            }
        }
        options.create_if_missing = false;
        if (mode == 0) {
            options.bloom_filter_layout = BloomFilterLayout::BlockBased;
        } else if (mode == 1) {
            options.bloom_filter_layout = BloomFilterLayout::FullTable;
        } else if (mode == 2) {
            options.bloom_bits_per_key = 10;
        } else if (mode == 3) {
            options.bloom_bits_per_key.reset();
        }
        auto reopened = Database::Open(options, directory.path());
        ASSERT_TRUE(reopened.has_value());
        const auto value = reopened->Get(AsBytes("key-42"));
        ASSERT_TRUE(value.has_value() && value->has_value());
        EXPECT_EQ(AsStringView(**value), std::string(1024, 'x'));
    }
}

TEST(PublicFilterTest, AutomaticCustomComparatorDoesNotInferByteEqualityFromItsName) {
    TemporaryDatabaseDirectory directory;
    auto options = CreatingOptions();
    options.write_buffer_size = 64 * 1024;
    options.compression = Compression::None;
    options.comparator = std::make_shared<AutomaticFilterComparator>();
    {
        auto opened = Database::Open(options, directory.path());
        ASSERT_TRUE(opened.has_value());
        ASSERT_TRUE(opened->Put(AsBytes("Alice"), AsBytes("value")).has_value());
        for (unsigned index = 0; index < 256; ++index) {
            const auto key = "filler-" + std::to_string(index);
            ASSERT_TRUE(opened->Put(AsBytes(key), AsBytes(std::string(1024, 'x'))).has_value());
        }
    }
    auto inspected = InspectFilters(directory.path());
    ASSERT_TRUE(inspected.has_value());
    EXPECT_GT(inspected->tables, 0U);
    EXPECT_TRUE(inspected->names.empty());
    auto reopened = Database::Open(options, directory.path());
    ASSERT_TRUE(reopened.has_value());
    const auto value = reopened->Get(AsBytes("ALICE"));
    ASSERT_TRUE(value.has_value() && value->has_value());
    EXPECT_EQ(AsStringView(**value), "value");
}

TEST(PublicDependencyTest, RetainsSuppliedServicesUntilTheLastIteratorOrSnapshot) {
    for (bool keep_snapshot : {false, true}) {
        SCOPED_TRACE(keep_snapshot);
        TemporaryDatabaseDirectory directory;
        std::filesystem::path path = directory.path();
        Options options = CreatingOptions();
        auto filesystem =
            CreateDefaultFileSystem(path, options.allow_weak_namespace_durability, false);
        ASSERT_TRUE(filesystem.has_value()) << filesystem.error().ToString();
        options.file_system = std::move(*filesystem);
        options.block_cache = CreateBlockCache(4096);
        options.clock = std::make_shared<dependency_test_support::CountingClock>();
        options.executor = std::make_shared<dependency_test_support::QueuedExecutor>();
        const std::array<std::weak_ptr<void>, 4> lifetimes{options.file_system, options.block_cache,
                                                           options.clock, options.executor};
        std::optional<Snapshot> snapshot;
        std::optional<Iterator> iterator;
        {
            auto opened = Database::Open(std::move(options), path);
            ASSERT_TRUE(opened.has_value()) << opened.error().ToString();
            ASSERT_TRUE(opened->Put(AsBytes("key"), AsBytes("value")).has_value());
            auto acquired_snapshot = opened->GetSnapshot();
            ASSERT_TRUE(acquired_snapshot.has_value());
            snapshot.emplace(std::move(*acquired_snapshot));
            auto acquired_iterator = opened->NewIterator({.snapshot = &*snapshot});
            ASSERT_TRUE(acquired_iterator.has_value());
            iterator.emplace(std::move(*acquired_iterator));
        }

        for (const auto& lifetime : lifetimes) {
            EXPECT_FALSE(lifetime.expired());
        }
        if (keep_snapshot) {
            iterator.reset();
        } else {
            snapshot.reset();
            ASSERT_TRUE(iterator->SeekToFirst().has_value());
            ASSERT_TRUE(iterator->valid());
            EXPECT_EQ(AsStringView(iterator->value()), "value");
        }
        for (const auto& lifetime : lifetimes) {
            EXPECT_FALSE(lifetime.expired());
        }
        iterator.reset();
        snapshot.reset();
        for (const auto& lifetime : lifetimes) {
            EXPECT_TRUE(lifetime.expired());
        }
    }
}

TEST(PublicDependencyTest, UsesInjectedSchedulingClockAndCacheAcrossReopen) {
    TemporaryDatabaseDirectory directory;
    std::filesystem::path path = directory.path();
    auto executor = std::make_shared<dependency_test_support::QueuedExecutor>();
    auto clock = std::make_shared<dependency_test_support::CountingClock>();
    Options options = CreatingOptions();
    options.write_buffer_size = 64 * 1024;
    options.allow_mmap_reads = false;
    options.compression = Compression::None;
    options.executor = executor;
    options.clock = clock;
    options.block_cache = CreateBlockCache(4 * 1024 * 1024);
    auto filesystem = CreateDefaultFileSystem(path, options.allow_weak_namespace_durability, false);
    ASSERT_TRUE(filesystem.has_value()) << filesystem.error().ToString();
    options.file_system = std::move(*filesystem);
    {
        auto opened = Database::Open(options, path);
        ASSERT_TRUE(opened.has_value()) << opened.error().ToString();
        dependency_test_support::DrainExecutor drain(*executor);
        for (int generation = 0; generation < 12; ++generation) {
            const std::string value(80 * 1024, static_cast<char>('a' + generation));
            ASSERT_TRUE(opened->Put(AsBytes("key"), AsBytes(value)).has_value());
            static_cast<void>(executor->RunOne());
        }
        EXPECT_GT(executor->scheduled(), 0U);
        EXPECT_GT(clock->sleep_count(), 0U);
    }
    {
        auto reopened = Database::Open(options, path);
        ASSERT_TRUE(reopened.has_value()) << reopened.error().ToString();
        dependency_test_support::DrainExecutor drain(*executor);
        executor->RunAll();
        const auto value = reopened->Get(AsBytes("key"));
        ASSERT_TRUE(value.has_value() && value->has_value());
        EXPECT_EQ(AsStringView(**value), std::string(80 * 1024, 'l'));
        EXPECT_GT(BlockCacheCharge(*options.block_cache), 0U);
    }
}

TEST(PublicDependencyTest, SharesOneCacheAcrossDatabasesWithoutMixingValues) {
    for (const auto policy :
         {BlockCachePolicy::Automatic, BlockCachePolicy::Lru, BlockCachePolicy::Clock}) {
        SCOPED_TRACE(static_cast<int>(policy));
        TemporaryDatabaseDirectory directory;
        ASSERT_TRUE(std::filesystem::create_directory(directory.path()));
        Options options = CreatingOptions();
        options.allow_mmap_reads = false;
        auto cache = CreateBlockCacheWithOptions({.capacity = 1024 * 1024, .policy = policy});
        ASSERT_TRUE(cache.has_value()) << cache.error().ToString();
        options.block_cache = std::move(*cache);
        const auto first_path = directory.path() / "first";
        const auto second_path = directory.path() / "second";
        {
            auto first = Database::Open(options, first_path);
            auto second = Database::Open(options, second_path);
            ASSERT_TRUE(first.has_value()) << first.error().ToString();
            ASSERT_TRUE(second.has_value()) << second.error().ToString();
            ASSERT_TRUE(first->Put(AsBytes("key"), AsBytes("first")).has_value());
            ASSERT_TRUE(second->Put(AsBytes("key"), AsBytes("other")).has_value());
        }
        auto first = Database::Open(options, first_path);
        auto second = Database::Open(options, second_path);
        ASSERT_TRUE(first.has_value()) << first.error().ToString();
        ASSERT_TRUE(second.has_value()) << second.error().ToString();

        for (int pass = 0; pass < 2; ++pass) {
            const auto first_value = first->Get(AsBytes("key"));
            const auto second_value = second->Get(AsBytes("key"));
            ASSERT_TRUE(first_value.has_value() && first_value->has_value());
            ASSERT_TRUE(second_value.has_value() && second_value->has_value());
            EXPECT_EQ(AsStringView(**first_value), "first");
            EXPECT_EQ(AsStringView(**second_value), "other");
        }
        EXPECT_GT(BlockCacheCharge(*options.block_cache), 0U);
    }
}

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
        const std::size_t batch_size = batch.ApproximateSize();
        ASSERT_TRUE(database.Write(batch, write_options).has_value());
        EXPECT_EQ(batch.ApproximateSize(), batch_size);

        WriteBatch exclusive;
        ASSERT_TRUE(exclusive.Put(AsBytes("c"), AsBytes("3")).has_value());
        const std::size_t exclusive_size = exclusive.ApproximateSize();
        ASSERT_TRUE(database.WriteExclusive(exclusive).has_value());
        EXPECT_EQ(exclusive.ApproximateSize(), exclusive_size);

        const auto deleted = database.Get(AsBytes("a"));
        ASSERT_TRUE(deleted.has_value());
        EXPECT_FALSE(deleted->has_value());
        const auto value = database.Get(AsBytes("b"));
        ASSERT_TRUE(value.has_value() && value->has_value());
        EXPECT_EQ(Text(**value), "2");
        const auto exclusive_value = database.Get(AsBytes("c"));
        ASSERT_TRUE(exclusive_value.has_value() && exclusive_value->has_value());
        EXPECT_EQ(Text(**exclusive_value), "3");
        ASSERT_TRUE(database.Delete(AsBytes("missing")).has_value());
    }

    Result<Database> reopened = Database::Open(OpeningOptions(), directory.path());
    ASSERT_TRUE(reopened.has_value()) << reopened.error().ToString();
    const auto value = reopened->Get(AsBytes("b"));
    ASSERT_TRUE(value.has_value() && value->has_value());
    EXPECT_EQ(Text(**value), "2");
    const auto exclusive_value = reopened->Get(AsBytes("c"));
    ASSERT_TRUE(exclusive_value.has_value() && exclusive_value->has_value());
    EXPECT_EQ(Text(**exclusive_value), "3");
}

TEST(PublicDatabaseTest, ReusesCallerOutputAndLeavesItUnchangedWhenAbsent) {
    TemporaryDatabaseDirectory directory;
    Result<Database> opened = Database::Open(CreatingOptions(), directory.path());
    ASSERT_TRUE(opened.has_value()) << opened.error().ToString();
    Database database = std::move(*opened);
    ASSERT_TRUE(database.Put(AsBytes("value"), AsBytes("stored")).has_value());
    ASSERT_TRUE(database.Put(AsBytes("deleted"), AsBytes("old")).has_value());
    ASSERT_TRUE(database.Delete(AsBytes("deleted")).has_value());
    ASSERT_TRUE(database.Put(AsBytes("empty"), {}).has_value());

    std::vector<std::byte> output;
    output.reserve(64);
    const ByteView sentinel = AsBytes("sentinel");
    output.assign(sentinel.begin(), sentinel.end());
    const std::size_t capacity = output.capacity();
    const std::byte* const storage = output.data();

    const Result<bool> found = database.Get(AsBytes("value"), output);
    ASSERT_TRUE(found.has_value() && *found);
    EXPECT_EQ(Text(output), "stored");
    EXPECT_EQ(output.capacity(), capacity);
    EXPECT_EQ(output.data(), storage);

    const ByteView unchanged_bytes = AsBytes("unchanged");
    output.assign(unchanged_bytes.begin(), unchanged_bytes.end());
    const std::vector<std::byte> unchanged = output;
    const Result<bool> missing = database.Get(AsBytes("missing"), output);
    ASSERT_TRUE(missing.has_value());
    EXPECT_FALSE(*missing);
    EXPECT_EQ(output, unchanged);
    EXPECT_EQ(output.capacity(), capacity);

    const Result<bool> deleted = database.Get(AsBytes("deleted"), output);
    ASSERT_TRUE(deleted.has_value());
    EXPECT_FALSE(*deleted);
    EXPECT_EQ(output, unchanged);

    const Result<bool> empty = database.Get(AsBytes("empty"), output);
    ASSERT_TRUE(empty.has_value() && *empty);
    EXPECT_TRUE(output.empty());
    EXPECT_EQ(output.capacity(), capacity);

    const auto owning = database.Get(AsBytes("value"));
    ASSERT_TRUE(owning.has_value() && owning->has_value());
    EXPECT_EQ(Text(**owning), "stored");

    Result<Snapshot> snapshot = database.GetSnapshot();
    ASSERT_TRUE(snapshot.has_value()) << snapshot.error().ToString();
    ASSERT_TRUE(database.Put(AsBytes("value"), AsBytes("new")).has_value());
    ReadOptions snapshot_read{.snapshot = &*snapshot};
    const Result<bool> old = database.Get(AsBytes("value"), output, snapshot_read);
    ASSERT_TRUE(old.has_value() && *old);
    EXPECT_EQ(Text(output), "stored");
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

TEST(PublicDatabaseTest, ReportsOwningDatabaseStateAndRetainedSnapshotRegistration) {
    TemporaryDatabaseDirectory directory;
    Result<Database> opened = Database::Open(CreatingOptions(), directory.path());
    ASSERT_TRUE(opened.has_value()) << opened.error().ToString();
    Database database = std::move(*opened);

    Result<DatabaseStateSnapshot> initial = database.GetState();
    ASSERT_TRUE(initial.has_value()) << initial.error().ToString();
    EXPECT_EQ(initial->last_sequence, 0U);
    EXPECT_EQ(initial->snapshot_count, 0U);
    EXPECT_FALSE(initial->oldest_snapshot_sequence.has_value());
    EXPECT_EQ(initial->write_queue_depth, 0U);
    EXPECT_GT(initial->mutable_memtable_bytes, 0U);
    EXPECT_FALSE(initial->immutable_memtable_bytes.has_value());
    EXPECT_EQ(initial->protected_output_count, 0U);
    EXPECT_FALSE(initial->background_work_scheduled);
    EXPECT_FALSE(initial->sticky_error.has_value());
    for (const DatabaseLevelState& level : initial->levels) {
        EXPECT_EQ(level.file_count, 0U);
        EXPECT_EQ(level.file_bytes, 0U);
    }

    const std::string large(5000, 'v');
    ASSERT_TRUE(database.Put(AsBytes("a"), AsBytes(large)).has_value());
    Result<DatabaseStateSnapshot> written = database.GetState();
    ASSERT_TRUE(written.has_value()) << written.error().ToString();
    EXPECT_EQ(written->last_sequence, 1U);
    EXPECT_GT(written->mutable_memtable_bytes, initial->mutable_memtable_bytes);

    std::optional<Iterator> retained_iterator;
    {
        Result<Snapshot> snapshot = database.GetSnapshot();
        ASSERT_TRUE(snapshot.has_value()) << snapshot.error().ToString();
        Result<DatabaseStateSnapshot> snapshotted = database.GetState();
        ASSERT_TRUE(snapshotted.has_value()) << snapshotted.error().ToString();
        EXPECT_EQ(snapshotted->snapshot_count, 1U);
        EXPECT_EQ(snapshotted->oldest_snapshot_sequence, 1U);

        ReadOptions options{.snapshot = &*snapshot};
        Result<Iterator> iterator = database.NewIterator(options);
        ASSERT_TRUE(iterator.has_value()) << iterator.error().ToString();
        retained_iterator.emplace(std::move(*iterator));
    }

    Result<DatabaseStateSnapshot> retained = database.GetState();
    ASSERT_TRUE(retained.has_value()) << retained.error().ToString();
    EXPECT_EQ(retained->snapshot_count, 1U);
    EXPECT_EQ(retained->oldest_snapshot_sequence, 1U);
    retained_iterator.reset();

    Result<DatabaseStateSnapshot> released = database.GetState();
    ASSERT_TRUE(released.has_value()) << released.error().ToString();
    EXPECT_EQ(released->snapshot_count, 0U);
    EXPECT_FALSE(released->oldest_snapshot_sequence.has_value());

    const DatabaseStateSnapshot saved = *released;
    ASSERT_TRUE(database.Put(AsBytes("b"), AsBytes("later")).has_value());
    Result<DatabaseStateSnapshot> later = database.GetState();
    ASSERT_TRUE(later.has_value()) << later.error().ToString();
    EXPECT_EQ(later->last_sequence, 2U);
    EXPECT_EQ(saved.last_sequence, 1U);
    EXPECT_GT(saved.mutable_memtable_bytes, 0U);
}

TEST(PublicDatabaseTest, DatabaseStateOutlivesItsDatabase) {
    TemporaryDatabaseDirectory directory;
    Options options = CreatingOptions();
    options.write_buffer_size = 64U << 10U;
    {
        Result<Database> opened = Database::Open(options, directory.path());
        ASSERT_TRUE(opened.has_value()) << opened.error().ToString();
        const std::string large(80'000, 'x');
        ASSERT_TRUE(opened->Put(AsBytes("large"), AsBytes(large)).has_value());
        ASSERT_TRUE(opened->Put(AsBytes("trigger"), AsBytes("value")).has_value());
    }

    DatabaseStateSnapshot saved;
    std::optional<std::size_t> nonempty_level;
    DatabaseLevelState projected_level;
    {
        Result<Database> opened = Database::Open(OpeningOptions(), directory.path());
        ASSERT_TRUE(opened.has_value()) << opened.error().ToString();
        Result<DatabaseStateSnapshot> state = opened->GetState();
        ASSERT_TRUE(state.has_value()) << state.error().ToString();
        saved = *state;
        for (std::size_t level = 0; level < state->levels.size(); ++level) {
            if (state->levels[level].file_count != 0) {
                nonempty_level = level;
                projected_level = state->levels[level];
                break;
            }
        }
    }

    ASSERT_TRUE(nonempty_level.has_value());
    EXPECT_GT(projected_level.file_count, 0U);
    EXPECT_GT(projected_level.file_bytes, 0U);
    EXPECT_EQ(saved.levels[*nonempty_level].file_count, projected_level.file_count);
    EXPECT_EQ(saved.levels[*nonempty_level].file_bytes, projected_level.file_bytes);
    EXPECT_EQ(saved.last_sequence, 2U);
    EXPECT_GT(saved.mutable_memtable_bytes, 0U);
    EXPECT_FALSE(saved.sticky_error.has_value());
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
    ExpectInvalid(first.WriteExclusive(valid_batch));
    ExpectInvalid(first.Get(AsBytes("a")));
    ExpectInvalid(first.NewIterator());
    ExpectInvalid(first.GetSnapshot());
    ExpectInvalid(first.GetState());

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
    ExpectInvalid(assigned.WriteExclusive(moved_batch_source));
    EXPECT_TRUE(assigned.Write(moved_batch).has_value());
    EXPECT_TRUE(assigned.WriteExclusive(moved_batch).has_value());
}

TEST(PublicDatabaseTest, PreparesSnapshotsConsistentlyForGetsAndIterators) {
    TemporaryDatabaseDirectory first_directory;
    TemporaryDatabaseDirectory second_directory;
    Result<Database> first = Database::Open(CreatingOptions(), first_directory.path());
    Result<Database> second = Database::Open(CreatingOptions(), second_directory.path());
    ASSERT_TRUE(first.has_value()) << first.error().ToString();
    ASSERT_TRUE(second.has_value()) << second.error().ToString();
    ASSERT_TRUE(first->Put(AsBytes("a"), AsBytes("old")).has_value());
    Result<Snapshot> original = first->GetSnapshot();
    Result<Snapshot> foreign = second->GetSnapshot();
    ASSERT_TRUE(original.has_value());
    ASSERT_TRUE(foreign.has_value());
    Snapshot retained = std::move(*original);
    ASSERT_TRUE(first->Put(AsBytes("a"), AsBytes("new")).has_value());
    const Snapshot* snapshots[] = {nullptr, &retained, &*original, &*foreign};
    const ByteView unchanged = AsBytes("unchanged");

    for (const bool fill_cache : {false, true}) {
        SCOPED_TRACE(fill_cache);
        for (const Snapshot* snapshot : snapshots) {
            const ReadOptions options{.snapshot = snapshot, .fill_cache = fill_cache};
            std::vector<std::byte> value(unchanged.begin(), unchanged.end());
            const Result<bool> found = first->Get(AsBytes("a"), value, options);
            const auto owning = first->Get(AsBytes("a"), options);
            auto iterator = first->NewIterator(options);
            if (snapshot == &*original || snapshot == &*foreign) {
                ASSERT_FALSE(found.has_value());
                ASSERT_FALSE(owning.has_value());
                ASSERT_FALSE(iterator.has_value());
                EXPECT_EQ(found.error().code(), ErrorCode::InvalidArgument);
                EXPECT_EQ(owning.error().code(), found.error().code());
                EXPECT_EQ(iterator.error().code(), found.error().code());
                EXPECT_EQ(owning.error().message(), found.error().message());
                EXPECT_EQ(iterator.error().message(), found.error().message());
                EXPECT_EQ(Text(value), "unchanged");
                continue;
            }
            const std::string_view expected = snapshot == nullptr ? "new" : "old";
            ASSERT_TRUE(found.has_value() && *found);
            ASSERT_TRUE(owning.has_value() && owning->has_value());
            ASSERT_TRUE(iterator.has_value()) << iterator.error().ToString();
            EXPECT_EQ(Text(value), expected);
            EXPECT_EQ(Text(**owning), expected);
            ASSERT_TRUE(iterator->SeekToFirst().has_value());
            ASSERT_TRUE(iterator->valid());
            EXPECT_EQ(Text(iterator->key()), "a");
            EXPECT_EQ(Text(iterator->value()), expected);
        }
    }
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
    const Result<Database> iterator_locked =
        Database::Open(OpeningOptions(), iterator_directory.path());
    ASSERT_FALSE(iterator_locked.has_value());
    EXPECT_EQ(iterator_locked.error().code(), ErrorCode::Busy);
    ASSERT_TRUE(iterator->SeekToFirst().has_value());
    ASSERT_TRUE(iterator->valid());
    EXPECT_EQ(Text(iterator->value()), "1");
    iterator.reset();
    EXPECT_TRUE(Database::Open(OpeningOptions(), iterator_directory.path()).has_value());

    TemporaryDatabaseDirectory snapshot_directory;
    std::optional<Snapshot> snapshot;
    {
        Result<Database> opened = Database::Open(CreatingOptions(), snapshot_directory.path());
        ASSERT_TRUE(opened.has_value()) << opened.error().ToString();
        Result<Snapshot> created = opened->GetSnapshot();
        ASSERT_TRUE(created.has_value()) << created.error().ToString();
        snapshot.emplace(std::move(*created));
    }
    const Result<Database> snapshot_locked =
        Database::Open(OpeningOptions(), snapshot_directory.path());
    ASSERT_FALSE(snapshot_locked.has_value());
    EXPECT_EQ(snapshot_locked.error().code(), ErrorCode::Busy);
    snapshot.reset();
    EXPECT_TRUE(Database::Open(OpeningOptions(), snapshot_directory.path()).has_value());
}

TEST(PublicDatabaseTest, ValidatesOptionsAndRetainsTheComparator) {
    TemporaryDatabaseDirectory invalid_directory;
    Options invalid = CreatingOptions();
    invalid.block_restart_interval = 0;
    const Result<Database> rejected = Database::Open(invalid, invalid_directory.path());
    ASSERT_FALSE(rejected.has_value());
    EXPECT_EQ(rejected.error().code(), ErrorCode::InvalidArgument);
    EXPECT_EQ(rejected.error().message(), "block_restart_interval must be at least one");
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
        EXPECT_EQ(result.error().message(), "compression type is invalid");
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
        EXPECT_EQ(result.error().message(), "Zstd compression level is outside [-5, 22]");
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

TEST(PublicDatabaseTest, AcceptsBothZstdLevelEndpoints) {
    for (const int level : {-5, 22}) {
        SCOPED_TRACE(level);
        TemporaryDatabaseDirectory directory;
        Options options = CreatingOptions();
        options.compression = Compression::Zstd;
        options.zstd_compression_level = level;

        const auto opened = Database::Open(options, directory.path());

        ASSERT_TRUE(opened.has_value()) << opened.error().ToString();
    }
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
        } else {
            options.zstd_compression_level = 23;
        }
        {
            Result<Database> opened = Database::Open(options, directory.path());
            ASSERT_TRUE(opened.has_value()) << opened.error().ToString();
            const std::string large(std::size_t{70} << 10U, 'v');
            ASSERT_TRUE(opened->Put(AsBytes("large"), AsBytes(large)).has_value());
            ASSERT_TRUE(opened->Put(AsBytes("trigger"), AsBytes("1")).has_value());
        }

        Result<Database> reopened = Database::Open(OpeningOptions(), directory.path());
        ASSERT_TRUE(reopened.has_value()) << reopened.error().ToString();
        const auto value = reopened->Get(AsBytes("large"));
        ASSERT_TRUE(value.has_value() && value->has_value());
        EXPECT_EQ(value->value().size(), std::size_t{70} << 10U);
    }
}

}  // namespace
}  // namespace modern_leveldb
