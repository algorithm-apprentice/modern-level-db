#include "engine/table_cache.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <filesystem>
#include <functional>
#include <initializer_list>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <semaphore>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#include "format/internal_key.h"
#include "metadata/filenames.h"
#include "modern_leveldb/base/bytes.h"
#include "modern_leveldb/base/coding.h"
#include "modern_leveldb/base/comparator.h"
#include "modern_leveldb/base/hash.h"
#include "modern_leveldb/base/result.h"
#include "platform/file_system.h"
#include "table/bloom_filter.h"
#include "table/table.h"
#include "table/table_builder.h"

namespace modern_leveldb {
namespace {

static_assert(!std::is_copy_constructible_v<TableCache>);
static_assert(!std::is_move_constructible_v<TableCache>);
static_assert(!std::is_copy_constructible_v<TableCache::Handle>);
static_assert(std::is_nothrow_move_constructible_v<TableCache::Handle>);
static_assert(std::is_nothrow_move_assignable_v<TableCache::Handle>);
static_assert(
    std::is_constructible_v<TableCache, FileSystem&, std::filesystem::path,
                            const InternalKeyComparator&, const TableOptions&, std::size_t>);
static_assert(!std::is_constructible_v<TableCache, FileSystem&, std::filesystem::path,
                                       InternalKeyComparator, const TableOptions&, std::size_t>);

struct FileHooks {
    std::mutex mutex;
    std::function<void(std::uint64_t)> opened;
    std::function<void(std::uint64_t, std::uint64_t)> read;
    std::function<void(std::uint64_t)> closed;
};

// Pausers must finish before destruction; ScheduleCleanup enforces that on exits.
class OneShotGate {
public:
    ~OneShotGate() { Open(); }

    void Pause() {
        if (!arrived_.exchange(true)) {
            reached_.release();
            released_.acquire();
        }
    }

    [[nodiscard]] bool Wait() { return reached_.try_acquire_for(std::chrono::seconds(5)); }

    void Open() {
        if (!opened_.exchange(true)) {
            released_.release();
        }
    }

private:
    std::atomic<bool> arrived_ = false;
    std::atomic<bool> opened_ = false;
    std::binary_semaphore reached_{0};
    std::binary_semaphore released_{0};
};

class MemoryRandomAccessFile final : public RandomAccessFile {
public:
    MemoryRandomAccessFile(std::shared_ptr<const std::vector<std::byte>> data,
                           std::shared_ptr<std::atomic<int>> reads,
                           std::shared_ptr<std::atomic<int>> live, std::uint64_t number,
                           std::shared_ptr<FileHooks> hooks)
        : data_(std::move(data)),
          reads_(std::move(reads)),
          live_(std::move(live)),
          number_(number),
          hooks_(std::move(hooks)) {
        ++*live_;
    }

    ~MemoryRandomAccessFile() override {
        std::function<void(std::uint64_t)> closed;
        {
            const std::lock_guard lock(hooks_->mutex);
            closed = hooks_->closed;
        }
        if (closed) {
            closed(number_);
        }
        --*live_;
    }

    Result<std::size_t> Read(std::uint64_t offset, MutableByteView output) const override {
        ++*reads_;
        std::function<void(std::uint64_t, std::uint64_t)> read;
        {
            const std::lock_guard lock(hooks_->mutex);
            read = hooks_->read;
        }
        if (read) {
            read(number_, offset);
        }
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
    std::shared_ptr<std::atomic<int>> live_;
    std::uint64_t number_;
    std::shared_ptr<FileHooks> hooks_;
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
    [[nodiscard]] int live_files() const { return live_files_->load(); }

    void SetOpenHook(std::function<void(std::uint64_t)> hook) {
        const std::lock_guard lock(hooks_->mutex);
        hooks_->opened = std::move(hook);
    }

    void SetReadHook(std::function<void(std::uint64_t, std::uint64_t)> hook) {
        const std::lock_guard lock(hooks_->mutex);
        hooks_->read = std::move(hook);
    }

    void SetCloseHook(std::function<void(std::uint64_t)> hook) {
        const std::lock_guard lock(hooks_->mutex);
        hooks_->closed = std::move(hook);
    }

    Result<std::unique_ptr<RandomAccessFile>> OpenRandomAccess(
        const std::filesystem::path& path,
        std::optional<std::uint64_t> expected_size = std::nullopt) override {
        (void)expected_size;
        const auto number = ParseNativeFileName(path)->number;
        std::unique_ptr<RandomAccessFile> file;
        {
            std::lock_guard lock(mutex_);
            ++opens_[path];
            if (open_error_.has_value()) {
                return std::unexpected(*open_error_);
            }
            const auto found = files_.find(path);
            if (found == files_.end()) {
                return std::unexpected(Error::NotFound(path.string()));
            }
            file = std::make_unique<MemoryRandomAccessFile>(found->second, reads_, live_files_,
                                                            number, hooks_);
        }
        std::function<void(std::uint64_t)> opened;
        {
            const std::lock_guard lock(hooks_->mutex);
            opened = hooks_->opened;
        }
        if (opened) {
            opened(number);
        }
        return file;
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
    Status CreateDir(const std::filesystem::path&) override { return Unused(); }
    Status RemoveFile(const std::filesystem::path&) override { return Unused(); }
    Status RemoveDir(const std::filesystem::path&) override { return Unused(); }
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
    std::shared_ptr<std::atomic<int>> live_files_ = std::make_shared<std::atomic<int>>(0);
    std::shared_ptr<FileHooks> hooks_ = std::make_shared<FileHooks>();
};

class ScheduleCleanup {
public:
    ScheduleCleanup(std::initializer_list<OneShotGate*> gates,
                    std::initializer_list<std::jthread*> workers,
                    MemoryFileSystem* file_system = nullptr)
        : gates_(gates), workers_(workers), file_system_(file_system) {}
    ScheduleCleanup(const ScheduleCleanup&) = delete;
    ScheduleCleanup& operator=(const ScheduleCleanup&) = delete;
    ~ScheduleCleanup() {
        if (file_system_ != nullptr) {
            file_system_->SetOpenHook({});
            file_system_->SetReadHook({});
            file_system_->SetCloseHook({});
        }
        for (auto* gate : gates_) {
            gate->Open();
        }
        for (auto* worker : workers_) {
            if (worker->joinable()) {
                worker->join();
            }
        }
    }

private:
    std::vector<OneShotGate*> gates_;
    std::vector<std::jthread*> workers_;
    MemoryFileSystem* file_system_;
};

TEST(TableReaderScheduleTest, CleanupOpensEveryGateAndJoinsBeforeBorrowedStateCanBeDestroyed) {
    std::atomic<bool> completed = false;
    std::jthread worker;
    OneShotGate first;
    OneShotGate second;
    {
        ScheduleCleanup cleanup({&first, &second}, {&worker});
        worker = std::jthread([&] {
            first.Pause();
            second.Pause();
            completed.store(true);
        });
        ASSERT_TRUE(first.Wait());
    }
    EXPECT_FALSE(worker.joinable());
    EXPECT_TRUE(completed.load());
}

TEST(TableReaderScheduleTest, ExceptionCleanupJoinsBeforeCapturedStateCanBeDestroyed) {
    std::atomic<bool> completed = false;
    std::jthread worker;
    OneShotGate first;
    OneShotGate second;
    EXPECT_THROW(
        {
            ScheduleCleanup cleanup({&first, &second}, {&worker});
            worker = std::jthread([&] {
                first.Pause();
                second.Pause();
                completed.store(true);
            });
            if (!first.Wait()) {
                throw std::logic_error("worker did not reach its gate");
            }
            throw std::runtime_error("leave the assertion scope");
        },
        std::runtime_error);
    EXPECT_FALSE(worker.joinable());
    EXPECT_TRUE(completed.load());
}

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
template <typename Handle>
std::optional<std::string> TryLookup(const Handle& table, std::string_view user_key) {
    auto key = LookupKey::Create(AsBytes(user_key), 100);
    if (!key.has_value()) {
        return std::nullopt;
    }
    std::vector<std::byte> value;
    auto lookup = table->Get(*key, value);
    if (!lookup.has_value() || *lookup != TableLookupKind::Value) {
        return std::nullopt;
    }
    return std::string(AsStringView(value));
}

std::vector<std::uint64_t> SameShardFiles(std::size_t count) {
    std::vector<std::uint64_t> result;
    std::array<bool, 64> used_hint_positions{};
    for (std::uint64_t candidate = 1; result.size() < count; ++candidate) {
        std::array<std::byte, 8> key{};
        EncodeFixed64(key, candidate);
        const auto hash = Hash32(key, 0);
        if (hash >> 28U == 0 && !used_hint_positions[hash & 63U]) {
            result.push_back(candidate);
            used_hint_positions[hash & 63U] = true;
        }
    }
    return result;
}

Result<TableLookupKind> PointRead(TableCache& cache, std::uint64_t number, std::uint64_t size,
                                  std::vector<std::byte>& value, SequenceNumber sequence = 100,
                                  const TableReadOptions& options = {}) {
    const auto key = LookupKey::CreateTrusted(AsBytes("key"), sequence);
    return cache.Get(number, size, key, value, options);
}

class TableCacheTest : public testing::Test {
protected:
    // Writes a table whose only entry maps "key" to the value as the file of the
    // number, and returns the table's size.
    std::uint64_t WriteTable(std::uint64_t number, std::string_view value,
                             const TableBuilderOptions& options = {}) {
        return WriteTableAt(directory_, number, value, options);
    }

    std::uint64_t WriteTableAt(const std::filesystem::path& directory, std::uint64_t number,
                               std::string_view value, const TableBuilderOptions& options = {}) {
        std::vector<std::byte> data;
        TableBuilder builder(std::make_unique<MemoryWritableFile>(data), comparator_, options);
        auto key = InternalKey::Create(AsBytes("key"), 1, ValueKind::Value);
        EXPECT_TRUE(key.has_value());
        EXPECT_TRUE(builder.Add(key->encoded(), AsBytes(value)).has_value());
        EXPECT_TRUE(builder.Finish().has_value());
        const std::uint64_t size = data.size();
        file_system_.Write(TableFileName(directory, number), std::move(data));
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

TEST_F(TableCacheTest, HandlesMoveAcrossImmutableOwnershipModesWithoutLosingTheirOwnPin) {
    const auto size = WriteTable(7, "seven");
    TableCache unique(file_system_, directory_, comparator_, {}, 16, false);
    TableCache shared(file_system_, directory_, comparator_, {}, 16, true);
    auto first = Found(unique.Find(7, size));
    auto second = Found(shared.Find(7, size));
    const auto* shared_table = &second.value();
    first = std::move(second);
    EXPECT_EQ(&first.value(), shared_table);
    auto third = Found(unique.Find(7, size));
    const auto* unique_table = &third.value();
    second = std::move(third);
    EXPECT_EQ(&second.value(), unique_table);
    unique.Evict(7);
    shared.Evict(7);
    EXPECT_EQ(TryLookup(first, "key"), "seven");
    EXPECT_EQ(TryLookup(second, "key"), "seven");
    auto* same = &first;
    first = std::move(*same);
    EXPECT_EQ(TryLookup(first, "key"), "seven");
}

TEST_F(TableCacheTest, SharedOwnershipObservationDoesNotBecomeUniqueWhenPressureClosesHints) {
    const auto files = SameShardFiles(3);
    std::array<std::uint64_t, 3> sizes{};
    for (std::size_t index = 0; index < files.size(); ++index) {
        sizes[index] = WriteTable(files[index], std::to_string(index));
    }
    for (const bool shared : {false, true}) {
        SCOPED_TRACE(shared);
        TableCache cache(file_system_, directory_, comparator_, {}, 16, shared);
        EXPECT_EQ(cache.uses_shared_readers(), shared);
        std::vector<std::byte> value;
        for (std::size_t index = 0; index < files.size(); ++index) {
            ASSERT_EQ(PointRead(cache, files[index], sizes[index], value), TableLookupKind::Value);
            EXPECT_EQ(cache.uses_shared_readers(), shared);
        }
        EXPECT_EQ(cache.uses_shared_readers(), shared);
    }
}

TEST_F(TableCacheTest, UniquePointFindRetainsItsOriginalTypedPinAcrossEvictionAndReplacement) {
    const auto old_size = WriteTable(7, "old");
    TableCache cache(file_system_, directory_, comparator_, {}, 16, false);
    ASSERT_FALSE(cache.uses_shared_readers());
    auto old = cache.FindUniqueForPoint(7, old_size);
    ASSERT_TRUE(old.has_value());
    const auto cached = cache.FindUniqueForPoint(7, old_size + 1);
    ASSERT_TRUE(cached.has_value());
    EXPECT_EQ(&old->value(), &cached->value());
    EXPECT_EQ(Opens(7), 1);
    cache.Evict(7);
    const auto new_size = WriteTable(7, "new");
    const auto replacement = cache.FindUniqueForPoint(7, new_size);
    ASSERT_TRUE(replacement.has_value());
    EXPECT_EQ(TryLookup(*old, "key"), "old");
    EXPECT_EQ(TryLookup(*cached, "key"), "old");
    EXPECT_EQ(TryLookup(*replacement, "key"), "new");
    EXPECT_EQ(Opens(7), 2);
}

TEST_F(TableCacheTest, FindContractsRetainTheirOriginalMeaningInBothOwnershipModes) {
    const auto files = SameShardFiles(3);
    for (const bool shared : {false, true}) {
        SCOPED_TRACE(shared);
        const auto a = WriteTable(files[0], "old");
        const auto b = WriteTable(files[1], "b");
        const auto c = WriteTable(files[2], "c");
        TableCache cache(file_system_, directory_, comparator_, {}, 16, shared);
        const auto old = Found(cache.Find(files[0], a));
        const auto a_opens = Opens(files[0]);
        static_cast<void>(Found(cache.Find(files[0], a + 1)));
        EXPECT_EQ(Opens(files[0]), a_opens);
        static_cast<void>(Found(cache.Find(files[1], b)));
        static_cast<void>(Found(cache.Find(files[2], c)));
        EXPECT_EQ(TryLookup(old, "key"), "old");
        static_cast<void>(Found(cache.Find(files[0], a + 1)));
        EXPECT_EQ(Opens(files[0]), a_opens);
        cache.Evict(files[0]);
        const auto replaced_size = WriteTable(files[0], "new");
        const auto replacement = Found(cache.Find(files[0], replaced_size));
        EXPECT_EQ(TryLookup(old, "key"), "old");
        EXPECT_EQ(TryLookup(replacement, "key"), "new");
        EXPECT_EQ(Opens(files[0]), a_opens + 1);
    }
    for (const bool shared : {false, true}) {
        SCOPED_TRACE(shared);
        TableCache cache(file_system_, directory_, comparator_, {}, 0, shared);
        file_system_.FailOpens(Error::Io("mode open failure"));
        ExpectError(cache.Find(7, 100), ErrorCode::Io);
        file_system_.FailOpens(std::nullopt);
        const auto size = WriteTable(7, "uncached");
        const auto before = Opens(7);
        const auto first = Found(cache.Find(7, size));
        const auto second = Found(cache.Find(7, size));
        EXPECT_EQ(Opens(7), before + 2);
        EXPECT_EQ(TryLookup(first, "key"), "uncached");
        EXPECT_EQ(TryLookup(second, "key"), "uncached");
    }
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

TEST_F(TableCacheTest, EvictedOldHandleSurvivesWhileReopenUsesRewrittenFile) {
    const std::uint64_t old_size = WriteTable(7, "old");
    TableCache cache(file_system_, directory_, comparator_, {}, 10);
    const TableCache::Handle old_table = Found(cache.Find(7, old_size));
    EXPECT_EQ(TryLookup(old_table, "key"), "old");
    EXPECT_EQ(Opens(7), 1);

    cache.Evict(7);
    const std::uint64_t new_size = WriteTable(7, "new");
    const TableCache::Handle new_table = Found(cache.Find(7, new_size));

    EXPECT_EQ(Opens(7), 2);
    EXPECT_EQ(TryLookup(old_table, "key"), "old");
    EXPECT_EQ(TryLookup(new_table, "key"), "new");
    EXPECT_EQ(TryLookup(Found(cache.Find(7, new_size)), "key"), "new");
    EXPECT_EQ(Opens(7), 2);
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
                if (!table.has_value() ||
                    TryLookup(*table, "key") != "value" + std::to_string(number)) {
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

TEST_F(TableCacheTest, PointReadsReuseTheReaderAndPreserveFillCacheControl) {
    const auto size = WriteTable(7, "seven");
    for (const bool reuse : {false, true}) {
        SCOPED_TRACE(reuse);
        const auto opens = Opens(7);
        BlockCache blocks(1 << 20);
        TableOptions options;
        options.block_cache = &blocks;
        TableCache cache(file_system_, directory_, comparator_, options, 16, reuse);
        std::vector<std::byte> value;
        auto found = PointRead(cache, 7, size, value, 100, {.fill_cache = false});
        ASSERT_TRUE(found.has_value());
        EXPECT_EQ(*found, TableLookupKind::Value);
        EXPECT_EQ(AsStringView(value), "seven");
        EXPECT_EQ(blocks.total_charge(), 0U);
        found = PointRead(cache, 7, size + 100, value);
        ASSERT_TRUE(found.has_value());
        EXPECT_EQ(*found, TableLookupKind::Value);
        EXPECT_EQ(AsStringView(value), "seven");
        EXPECT_GT(blocks.total_charge(), 0U);
        EXPECT_EQ(Opens(7), opens + 1);
    }
}

TEST_F(TableCacheTest, ReusedReaderDoesNotCacheSequenceVisibilityOrTombstoneResults) {
    std::vector<std::byte> bytes;
    TableBuilder builder(std::make_unique<MemoryWritableFile>(bytes), comparator_, {});
    const auto deletion = InternalKey::Create(AsBytes("key"), 2, ValueKind::Deletion);
    const auto old = InternalKey::Create(AsBytes("key"), 1, ValueKind::Value);
    ASSERT_TRUE(deletion.has_value());
    ASSERT_TRUE(old.has_value());
    ASSERT_TRUE(builder.Add(deletion->encoded(), {}).has_value());
    ASSERT_TRUE(builder.Add(old->encoded(), AsBytes("old")).has_value());
    ASSERT_TRUE(builder.Finish().has_value());
    const auto size = bytes.size();
    file_system_.Write(TableFileName(directory_, 7), std::move(bytes));
    for (const bool reuse : {false, true}) {
        SCOPED_TRACE(reuse);
        const auto opens = Opens(7);
        TableCache cache(file_system_, directory_, comparator_, {}, 16, reuse);
        std::vector<std::byte> value;
        auto found = PointRead(cache, 7, size, value, 1);
        ASSERT_TRUE(found.has_value());
        EXPECT_EQ(*found, TableLookupKind::Value);
        EXPECT_EQ(AsStringView(value), "old");
        found = PointRead(cache, 7, size, value, 2);
        ASSERT_TRUE(found.has_value());
        EXPECT_EQ(*found, TableLookupKind::Deletion);
        EXPECT_EQ(AsStringView(value), "old");
        found = PointRead(cache, 7, size, value, 0);
        ASSERT_TRUE(found.has_value());
        EXPECT_EQ(*found, TableLookupKind::Missing);
        EXPECT_EQ(AsStringView(value), "old");
        EXPECT_EQ(Opens(7), opens + 1);
    }
}

TEST_F(TableCacheTest, CompletedEvictionInvalidatesAHintEvenWhileTheOldReaderIsPinned) {
    const auto old_size = WriteTable(7, "old");
    TableCache cache(file_system_, directory_, comparator_, {}, 32, true);
    std::vector<std::byte> value;
    ASSERT_EQ(PointRead(cache, 7, old_size, value), TableLookupKind::Value);
    const auto old = Found(cache.Find(7, old_size));
    cache.Evict(7);
    const auto new_size = WriteTable(7, "new");
    ASSERT_EQ(PointRead(cache, 7, new_size, value), TableLookupKind::Value);
    EXPECT_EQ(AsStringView(value), "new");
    EXPECT_EQ(TryLookup(old, "key"), "old");
    EXPECT_EQ(Opens(7), 2);
}

TEST_F(TableCacheTest, WeakHintsKeepNoFilesOpenAfterCacheDestruction) {
    const auto size = WriteTable(7, "seven");
    {
        TableCache cache(file_system_, directory_, comparator_, {}, 16, true);
        std::vector<std::byte> value;
        ASSERT_EQ(PointRead(cache, 7, size, value), TableLookupKind::Value);
        EXPECT_EQ(file_system_.live_files(), 1);
    }
    EXPECT_EQ(file_system_.live_files(), 0);
}

TEST_F(TableCacheTest, ZeroCapacityNeverRetainsReadersThroughHints) {
    const auto size = WriteTable(7, "seven");
    for (const bool reuse : {false, true}) {
        SCOPED_TRACE(reuse);
        const auto opens = Opens(7);
        TableCache cache(file_system_, directory_, comparator_, {}, 0, reuse);
        std::vector<std::byte> value;
        for (int attempt = 0; attempt < 3; ++attempt) {
            ASSERT_EQ(PointRead(cache, 7, size, value), TableLookupKind::Value);
            EXPECT_EQ(file_system_.live_files(), 0);
        }
        EXPECT_EQ(Opens(7), opens + 3);
    }
}

TEST_F(TableCacheTest, CompleteCacheIdentityDistinguishesCollidingHintsForTheSameFileNumber) {
    constexpr std::size_t Caches = 129;
    std::vector<std::unique_ptr<TableCache>> caches;
    std::vector<std::uint64_t> sizes;
    for (std::size_t index = 0; index < Caches; ++index) {
        const auto directory = directory_ / std::to_string(index);
        sizes.push_back(WriteTableAt(directory, 7, "database-" + std::to_string(index)));
        caches.push_back(std::make_unique<TableCache>(file_system_, directory, comparator_,
                                                      TableOptions{}, 16, true));
        std::vector<std::byte> value;
        ASSERT_EQ(PointRead(*caches.back(), 7, sizes.back(), value), TableLookupKind::Value);
        EXPECT_EQ(AsStringView(value), "database-" + std::to_string(index));
    }
    for (std::size_t index = 0; index < Caches; ++index) {
        std::vector<std::byte> value;
        ASSERT_EQ(PointRead(*caches[index], 7, sizes[index], value), TableLookupKind::Value);
        EXPECT_EQ(AsStringView(value), "database-" + std::to_string(index));
    }
    EXPECT_EQ(file_system_.live_files(), static_cast<int>(Caches));
    caches.clear();
    EXPECT_EQ(file_system_.live_files(), 0);
}

TEST_F(TableCacheTest, PressureHandoffAdmitsSkippedHistoryThenUsesOrdinaryPromotions) {
    const auto files = SameShardFiles(4);
    std::array<std::uint64_t, 4> sizes{};
    for (std::size_t index = 0; index < files.size(); ++index) {
        sizes[index] = WriteTable(files[index], "value-" + std::to_string(index));
    }
    for (const bool reuse : {false, true}) {
        SCOPED_TRACE(reuse);
        TableCache cache(file_system_, directory_, comparator_, {}, 32, reuse);
        std::vector<std::byte> value;
        for (const auto index : {0U, 1U, 0U, 2U}) {
            ASSERT_EQ(PointRead(cache, files[index], sizes[index], value), TableLookupKind::Value);
        }
        const auto victim = reuse ? 0U : 1U;
        const auto survivor = reuse ? 1U : 0U;
        const auto before_survivor = Opens(files[survivor]);
        const auto before_victim = Opens(files[victim]);
        {
            const auto retained = Found(cache.Find(files[survivor], sizes[survivor]));
            EXPECT_EQ(Opens(files[survivor]), before_survivor);
            static_cast<void>(Found(cache.Find(files[victim], sizes[victim])));
            EXPECT_EQ(Opens(files[victim]), before_victim + 1);
        }
        static_cast<void>(Found(cache.Find(files[victim], sizes[victim])));
        ASSERT_EQ(PointRead(cache, files[survivor], sizes[survivor], value),
                  TableLookupKind::Value);
        static_cast<void>(Found(cache.Find(files[3], sizes[3])));
        const auto survivor_opens = Opens(files[survivor]);
        const auto victim_opens = Opens(files[victim]);
        const auto retained = Found(cache.Find(files[survivor], sizes[survivor]));
        EXPECT_EQ(Opens(files[survivor]), survivor_opens);
        static_cast<void>(Found(cache.Find(files[victim], sizes[victim])));
        EXPECT_EQ(Opens(files[victim]), victim_opens + 1);
    }
}

TEST_F(TableCacheTest, PointReadErrorsRemainErrorsAndAreNotCachedAsAbsence) {
    const auto size = WriteTable(7, "seven");
    for (const bool reuse : {false, true}) {
        SCOPED_TRACE(reuse);
        const auto opens = Opens(7);
        TableCache cache(file_system_, directory_, comparator_, {}, 16, reuse);
        std::vector<std::byte> value{std::byte{0x5a}};
        file_system_.FailOpens(Error::Io("point open failure"));
        const auto failed = PointRead(cache, 7, size, value);
        ASSERT_FALSE(failed.has_value());
        EXPECT_EQ(failed.error().code(), ErrorCode::Io);
        EXPECT_EQ(AsStringView(value), "Z");
        file_system_.FailOpens(std::nullopt);
        ASSERT_EQ(PointRead(cache, 7, size, value), TableLookupKind::Value);
        EXPECT_EQ(AsStringView(value), "seven");
        EXPECT_EQ(Opens(7), opens + 2);
    }
}

TEST_F(TableCacheTest, FallbackReadPinsMappingButHintReadPinsOnlyTheReaderAcrossPressure) {
    const auto files = SameShardFiles(2);
    const auto a = WriteTable(files[0], "a");
    const auto b = WriteTable(files[1], "b");
    for (const auto& [reuse, prime_hint] :
         {std::pair{false, false}, std::pair{true, false}, std::pair{true, true}}) {
        SCOPED_TRACE(testing::Message() << "reuse=" << reuse << ", hint=" << prime_hint);
        TableCache cache(file_system_, directory_, comparator_, {}, 16, reuse);
        std::optional<Result<TableLookupKind>> result;
        std::exception_ptr error;
        std::vector<std::byte> output;
        std::jthread reader;
        OneShotGate data_read;
        std::atomic<int> reads = 0;
        file_system_.SetReadHook([&](std::uint64_t number, std::uint64_t offset) {
            if (number == files[0] && offset == 0 && ++reads == (prime_hint ? 2 : 1)) {
                data_read.Pause();
            }
        });
        ScheduleCleanup cleanup({&data_read}, {&reader}, &file_system_);
        reader = std::jthread([&] {
            try {
                if (prime_hint) {
                    result.emplace(PointRead(cache, files[0], a, output));
                }
                result.emplace(PointRead(cache, files[0], a, output));
            } catch (...) {
                error = std::current_exception();
            }
        });
        ASSERT_TRUE(data_read.Wait());
        std::vector<std::byte> other;
        ASSERT_EQ(PointRead(cache, files[1], b, other), TableLookupKind::Value);
        const auto still_indexed = cache.Find(files[0], a + 1);
        EXPECT_EQ(still_indexed.has_value(), !prime_hint);
        if (!still_indexed.has_value()) {
            EXPECT_EQ(still_indexed.error().code(), ErrorCode::Corruption);
        }
        EXPECT_GE(file_system_.live_files(), 2);
        data_read.Open();
        reader.join();
        ASSERT_FALSE(error);
        ASSERT_TRUE(result.has_value());
        ASSERT_EQ(*result, TableLookupKind::Value);
        EXPECT_EQ(AsStringView(output), "a");
    }
}

TEST_F(TableCacheTest, OldOpenAfterEvictMayRecacheButCannotRetagItsHintAsTheNewGeneration) {
    const auto files = SameShardFiles(3);
    const auto a = WriteTable(files[0], "old");
    const auto b = WriteTable(files[1], "b");
    const auto c = WriteTable(files[2], "c");
    TableCache cache(file_system_, directory_, comparator_, {}, 32, true);
    std::optional<Result<TableLookupKind>> result;
    std::exception_ptr error;
    std::vector<std::byte> value;
    std::jthread reader;
    OneShotGate open;
    OneShotGate second_read;
    file_system_.SetOpenHook([&](std::uint64_t number) {
        if (number == files[0]) {
            open.Pause();
        }
    });
    ScheduleCleanup cleanup({&open, &second_read}, {&reader}, &file_system_);
    reader = std::jthread([&] {
        try {
            result.emplace(PointRead(cache, files[0], a, value));
            second_read.Pause();
            result.emplace(PointRead(cache, files[0], a, value));
        } catch (...) {
            error = std::current_exception();
        }
    });
    ASSERT_TRUE(open.Wait());
    cache.Evict(files[0]);
    static_cast<void>(WriteTable(files[0], "new"));
    open.Open();
    ASSERT_TRUE(second_read.Wait());
    std::vector<std::byte> other;
    ASSERT_EQ(PointRead(cache, files[1], b, other), TableLookupKind::Value);
    second_read.Open();
    reader.join();
    ASSERT_FALSE(error);
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(*result, TableLookupKind::Value);
    EXPECT_EQ(AsStringView(value), "old");
    ASSERT_EQ(PointRead(cache, files[2], c, other), TableLookupKind::Value);
    const auto retained = Found(cache.Find(files[0], a));
    EXPECT_EQ(Opens(files[0]), 1);
    EXPECT_EQ(TryLookup(retained, "key"), "old");
}

TEST_F(TableCacheTest, InvalidationFollowsEraseEvenWhenNewReadsRecacheDuringClose) {
    const auto files = SameShardFiles(4);
    const auto old = WriteTable(files[0], "old");
    std::array<std::uint64_t, 4> sizes{};
    for (std::size_t index = 1; index < files.size(); ++index) {
        sizes[index] = WriteTable(files[index], "value");
    }
    TableCache cache(file_system_, directory_, comparator_, {}, 48, true);
    std::vector<std::byte> value;
    ASSERT_EQ(PointRead(cache, files[0], old, value), TableLookupKind::Value);
    std::jthread evictor;
    OneShotGate close;
    file_system_.SetCloseHook([&](std::uint64_t number) {
        if (number == files[0]) {
            close.Pause();
        }
    });
    ScheduleCleanup cleanup({&close}, {&evictor}, &file_system_);
    evictor = std::jthread([&] { cache.Evict(files[0]); });
    ASSERT_TRUE(close.Wait());
    sizes[0] = WriteTable(files[0], "new");
    ASSERT_EQ(PointRead(cache, files[0], sizes[0], value), TableLookupKind::Value);
    EXPECT_EQ(AsStringView(value), "new");
    close.Open();
    evictor.join();
    ASSERT_EQ(PointRead(cache, files[1], sizes[1], value), TableLookupKind::Value);
    ASSERT_EQ(PointRead(cache, files[0], sizes[0], value), TableLookupKind::Value);
    ASSERT_EQ(PointRead(cache, files[2], sizes[2], value), TableLookupKind::Value);
    static_cast<void>(Found(cache.Find(files[3], sizes[3])));
    const auto retained = Found(cache.Find(files[0], sizes[0]));
    EXPECT_EQ(Opens(files[0]), 2);
    EXPECT_EQ(TryLookup(retained, "key"), "new");
}

TEST_F(TableCacheTest, WeakThreadHintsOutliveCacheClosureWithoutRetainingFiles) {
    const auto size = WriteTable(7, "seven");
    auto cache = std::make_unique<TableCache>(file_system_, directory_, comparator_, TableOptions{},
                                              16, true);
    std::optional<Result<TableLookupKind>> result;
    std::vector<std::byte> value;
    std::jthread worker;
    OneShotGate thread_exit;
    ScheduleCleanup cleanup({&thread_exit}, {&worker});
    worker = std::jthread([&] {
        result.emplace(PointRead(*cache, 7, size, value));
        thread_exit.Pause();
    });
    ASSERT_TRUE(thread_exit.Wait());
    ASSERT_TRUE(result.has_value());
    ASSERT_EQ(*result, TableLookupKind::Value);
    EXPECT_EQ(file_system_.live_files(), 1);
    cache.reset();
    EXPECT_EQ(file_system_.live_files(), 0);
    thread_exit.Open();
    worker.join();
    EXPECT_EQ(file_system_.live_files(), 0);
}

TEST_F(TableCacheTest, DuplicateSuccessfulOpensCloseHintsBeforeEitherNewReaderIsReliedOn) {
    const auto files = SameShardFiles(2);
    const auto a = WriteTable(files[0], "a");
    const auto b = WriteTable(files[1], "b");
    TableCache cache(file_system_, directory_, comparator_, {}, 16, true);
    std::optional<Result<TableLookupKind>> first_result;
    std::optional<Result<TableLookupKind>> read_result;
    std::exception_ptr error;
    std::vector<std::byte> first_value;
    std::vector<std::byte> read_value;
    std::jthread opener;
    std::jthread reader;
    OneShotGate open;
    OneShotGate second_read;
    std::atomic<int> data_reads = 0;
    file_system_.SetOpenHook([&](std::uint64_t number) {
        if (number == files[0]) {
            open.Pause();
        }
    });
    ScheduleCleanup cleanup({&open, &second_read}, {&opener, &reader}, &file_system_);
    opener = std::jthread([&] {
        try {
            first_result.emplace(PointRead(cache, files[0], a, first_value));
        } catch (...) {
            error = std::current_exception();
        }
    });
    ASSERT_TRUE(open.Wait());
    std::vector<std::byte> other;
    ASSERT_EQ(PointRead(cache, files[0], a, other), TableLookupKind::Value);
    open.Open();
    opener.join();
    ASSERT_FALSE(error);
    ASSERT_TRUE(first_result.has_value());
    ASSERT_EQ(*first_result, TableLookupKind::Value);
    ASSERT_EQ(Opens(files[0]), 2);
    file_system_.SetReadHook([&](std::uint64_t number, std::uint64_t offset) {
        if (number == files[0] && offset == 0 && ++data_reads == 2) {
            second_read.Pause();
        }
    });
    reader = std::jthread([&] {
        try {
            read_result.emplace(PointRead(cache, files[0], a, read_value));
            read_result.emplace(PointRead(cache, files[0], a, read_value));
        } catch (...) {
            error = std::current_exception();
        }
    });
    ASSERT_TRUE(second_read.Wait());
    ASSERT_EQ(PointRead(cache, files[1], b, other), TableLookupKind::Value);
    const auto pinned = cache.Find(files[0], a + 1);
    EXPECT_TRUE(pinned.has_value());
    second_read.Open();
    reader.join();
    ASSERT_FALSE(error);
    ASSERT_TRUE(read_result.has_value());
    ASSERT_EQ(*read_result, TableLookupKind::Value);
    EXPECT_EQ(AsStringView(read_value), "a");
}

TEST(TableReaderReuseStateTest, OpenEventsIncludeTheBoundaryThenSaturatePermanently) {
    table_cache_detail::ReuseShard state;
    EXPECT_TRUE(state.enabled(3));
    for (std::size_t event = 1; event <= 3; ++event) {
        state.RecordOpen(3);
        EXPECT_EQ(state.opens.load(), event);
        EXPECT_TRUE(state.enabled(3));
    }
    for (int event = 0; event < 10; ++event) {
        state.RecordOpen(3);
        EXPECT_EQ(state.opens.load(), 4U);
        EXPECT_FALSE(state.enabled(3));
    }
    table_cache_detail::ReuseShard zero;
    EXPECT_FALSE(zero.enabled(0));
    zero.RecordOpen(0);
    EXPECT_EQ(zero.opens.load(), 1U);
    EXPECT_FALSE(zero.enabled(0));
}

TEST(TableReaderReuseStateTest, MaximumRoundedCapacityHasARepresentableClosedCount) {
    const auto capacity =
        ShardedLruCacheCore::ShardCapacity(std::numeric_limits<std::size_t>::max());
    ASSERT_LT(capacity, std::numeric_limits<std::size_t>::max());
    table_cache_detail::ReuseShard state;
    state.opens.store(capacity);
    state.RecordOpen(capacity);
    EXPECT_EQ(state.opens.load(), capacity + 1);
    EXPECT_FALSE(state.enabled(capacity));
}

TEST(TableReaderReuseStateTest, TerminalGenerationClosesTheGateAndNeverWraps) {
    table_cache_detail::ReuseShard state;
    state.RecordOpen(3);
    state.generation.store(std::numeric_limits<std::uint64_t>::max() - 1);
    state.Invalidate(3);
    EXPECT_EQ(state.generation.load(), std::numeric_limits<std::uint64_t>::max());
    EXPECT_FALSE(state.enabled(3));
    EXPECT_EQ(state.opens.load(), 4U);
    for (int attempt = 0; attempt < 10; ++attempt) {
        state.Invalidate(3);
        state.RecordOpen(3);
        EXPECT_EQ(state.generation.load(), std::numeric_limits<std::uint64_t>::max());
        EXPECT_EQ(state.opens.load(), 4U);
        EXPECT_FALSE(state.enabled(3));
    }
}

TEST(TableReaderReuseStateTest, CompletedEvictionCannotOvertakeAClosingEpochIntoAnAncientHint) {
    table_cache_detail::ReuseShard state;
    state.RecordOpen(1);
    state.generation.store(std::numeric_limits<std::uint64_t>::max() - 1);
    std::jthread delayed;
    OneShotGate prepared;
    ScheduleCleanup cleanup({&prepared}, {&delayed});
    delayed = std::jthread([&] {
        const auto observed = state.BeginInvalidation(1);
        prepared.Pause();
        state.CompleteInvalidation(observed, 1);
    });
    ASSERT_TRUE(prepared.Wait());
    EXPECT_FALSE(state.enabled(1));
    state.Invalidate(1);
    const auto completed_generation = state.generation.load(std::memory_order_acquire);
    EXPECT_EQ(completed_generation, std::numeric_limits<std::uint64_t>::max());
    EXPECT_FALSE(state.enabled(1));
    prepared.Open();
    delayed.join();
    EXPECT_EQ(state.generation.load(), completed_generation);
    EXPECT_EQ(state.opens.load(), 2U);
    EXPECT_FALSE(state.enabled(1));
}

TEST(TableReaderReuseStateTest, ConcurrentEvictionsCannotPublishAReusedEpochOrReopenTheGate) {
    table_cache_detail::ReuseShard state;
    state.RecordOpen(1);
    state.generation.store(std::numeric_limits<std::uint64_t>::max() - 1);
    std::barrier start(3);
    std::atomic<int> completed = 0;
    std::atomic<bool> terminal_with_open_gate = false;
    const auto evict = [&] {
        start.arrive_and_wait();
        state.Invalidate(1);
        state.RecordOpen(1);
        ++completed;
    };
    std::jthread first(evict);
    std::jthread second(evict);
    start.arrive_and_wait();
    while (completed.load() != 2) {
        const auto generation = state.generation.load(std::memory_order_acquire);
        if (generation == std::numeric_limits<std::uint64_t>::max() && state.enabled(1)) {
            terminal_with_open_gate.store(true);
        }
    }
    first.join();
    second.join();
    EXPECT_FALSE(terminal_with_open_gate.load());
    EXPECT_EQ(state.generation.load(), std::numeric_limits<std::uint64_t>::max());
    EXPECT_FALSE(state.enabled(1));
    EXPECT_EQ(state.opens.load(), 2U);
}

TEST(TableReaderReuseStateTest, ConcurrentOpenEventsCannotUndoSaturation) {
    table_cache_detail::ReuseShard state;
    constexpr std::size_t Capacity = 200000;
    std::barrier start(5);
    std::vector<std::jthread> workers;
    for (int worker = 0; worker < 4; ++worker) {
        workers.emplace_back([&] {
            start.arrive_and_wait();
            for (std::size_t event = 0; event < Capacity; ++event) {
                state.RecordOpen(Capacity);
            }
        });
    }
    start.arrive_and_wait();
    workers.clear();
    EXPECT_EQ(state.opens.load(), Capacity + 1);
    EXPECT_FALSE(state.enabled(Capacity));
}

}  // namespace
}  // namespace modern_leveldb
