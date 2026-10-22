#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstddef>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "modern_leveldb/block_cache.h"
#include "modern_leveldb/db.h"

namespace modern_leveldb {
namespace {

using ExpectedOwningRead = Result<std::optional<std::vector<std::byte>>>;
static_assert(std::is_same_v<OwningReadResult, ExpectedOwningRead>);
using BatchSignature = Result<std::vector<OwningReadResult>> (Database::*)(
    std::span<const ByteView>, const MultiGetOptions&);
static_assert(std::is_same_v<decltype(&Database::MultiGet), BatchSignature>);

class PublicMultiGetTest : public testing::Test {
protected:
    void SetUp() override {
        root_ = std::filesystem::temp_directory_path() /
                ("modern-leveldb-batch-" +
                 std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        std::filesystem::create_directories(root_);
        auto opened = Database::Open(Creating(), root_ / "first");
        ASSERT_TRUE(opened.has_value()) << opened.error().ToString();
        database_.emplace(std::move(*opened));
    }

    void TearDown() override {
        database_.reset();
        std::error_code error;
        std::filesystem::remove_all(root_, error);
        EXPECT_FALSE(error) << error.message();
    }

    static Options Creating() {
        Options options;
        options.create_if_missing = true;
        options.allow_mmap_reads = false;
        options.block_cache = CreateBlockCache(0);
#if defined(_WIN32)
        options.allow_weak_namespace_durability = true;
#endif
        return options;
    }

    static void ExpectValue(const OwningReadResult& result, std::string_view expected) {
        ASSERT_TRUE(result.has_value()) << result.error().ToString();
        ASSERT_TRUE(result->has_value());
        EXPECT_EQ(AsStringView(**result), expected);
    }

    std::filesystem::path root_;
    std::optional<Database> database_;
};

TEST(PublicMultiGetOptionsTest, DefaultsToFourBoundedReadersAndOrdinaryReadOptions) {
    const MultiGetOptions options;
    EXPECT_EQ(options.max_concurrency, 4U);
    EXPECT_EQ(options.read_options.snapshot, nullptr);
    EXPECT_TRUE(options.read_options.fill_cache);
}

TEST_F(PublicMultiGetTest, PreservesOrderDuplicatesBinaryKeysEmptyValuesAndAbsence) {
    const std::string binary("b\0\xff", 3);
    ASSERT_TRUE(database_->Put(AsBytes("a"), AsBytes("A")).has_value());
    ASSERT_TRUE(database_->Put(AsBytes(binary), AsBytes("binary")).has_value());
    ASSERT_TRUE(database_->Put({}, AsBytes("empty-key")).has_value());
    ASSERT_TRUE(database_->Put(AsBytes("empty-value"), {}).has_value());
    ASSERT_TRUE(database_->Put(AsBytes("deleted"), AsBytes("old")).has_value());
    ASSERT_TRUE(database_->Delete(AsBytes("deleted")).has_value());
    const std::array<ByteView, 7> keys{
        AsBytes("a"),       AsBytes(binary),    {},          AsBytes("empty-value"),
        AsBytes("missing"), AsBytes("deleted"), AsBytes("a")};
    for (const auto concurrency : {1U, 4U, 16U}) {
        MultiGetOptions options;
        options.max_concurrency = concurrency;
        auto read = database_->MultiGet(keys, options);
        ASSERT_TRUE(read.has_value());
        ASSERT_EQ(read->size(), keys.size());
        ExpectValue((*read)[0], "A");
        ExpectValue((*read)[1], "binary");
        ExpectValue((*read)[2], "empty-key");
        ExpectValue((*read)[3], "");
        ASSERT_TRUE((*read)[4].has_value());
        EXPECT_FALSE((*read)[4]->has_value());
        ASSERT_TRUE((*read)[5].has_value());
        EXPECT_FALSE((*read)[5]->has_value());
        ExpectValue((*read)[6], "A");
        (**(*read)[0])[0] = std::byte{'X'};
        ExpectValue((*read)[6], "A");
        EXPECT_EQ(database_->GetState().value().snapshot_count, 0U);
    }
}

TEST_F(PublicMultiGetTest, RetainsOneExplicitSnapshotAndUsesTheCurrentImplicitSequenceOtherwise) {
    ASSERT_TRUE(database_->Put(AsBytes("a"), AsBytes("old")).has_value());
    ASSERT_TRUE(database_->Put(AsBytes("b"), AsBytes("present")).has_value());
    auto snapshot = database_->GetSnapshot();
    ASSERT_TRUE(snapshot.has_value());
    ASSERT_TRUE(database_->Put(AsBytes("a"), AsBytes("new")).has_value());
    ASSERT_TRUE(database_->Delete(AsBytes("b")).has_value());
    const std::array keys{AsBytes("a"), AsBytes("b"), AsBytes("a")};
    MultiGetOptions options;
    options.read_options.snapshot = &*snapshot;
    auto old = database_->MultiGet(keys, options);
    ASSERT_TRUE(old.has_value());
    ExpectValue((*old)[0], "old");
    ExpectValue((*old)[1], "present");
    ExpectValue((*old)[2], "old");
    EXPECT_EQ(database_->GetState().value().snapshot_count, 1U);
    const auto current = database_->MultiGet(keys);
    ASSERT_TRUE(current.has_value());
    ExpectValue((*current)[0], "new");
    ASSERT_TRUE((*current)[1].has_value());
    EXPECT_FALSE((*current)[1]->has_value());
    ExpectValue((*current)[2], "new");
    EXPECT_EQ(database_->GetState().value().snapshot_count, 1U);
}

TEST_F(PublicMultiGetTest, ValidatesConcurrencyEvenForEmptyInputWithoutLeavingRegistrations) {
    const auto empty = database_->MultiGet({});
    ASSERT_TRUE(empty.has_value());
    EXPECT_TRUE(empty->empty());
    EXPECT_EQ(database_->GetState().value().snapshot_count, 0U);
    for (const auto invalid : {0U, 17U, 1000U}) {
        MultiGetOptions options;
        options.max_concurrency = invalid;
        const auto rejected = database_->MultiGet({}, options);
        ASSERT_FALSE(rejected.has_value());
        EXPECT_EQ(rejected.error().code(), ErrorCode::InvalidArgument);
        EXPECT_EQ(database_->GetState().value().snapshot_count, 0U);
    }
}

TEST_F(PublicMultiGetTest, RejectsMovedAndForeignSnapshotsEvenForEmptyInput) {
    auto snapshot = database_->GetSnapshot();
    ASSERT_TRUE(snapshot.has_value());
    auto retained = std::move(*snapshot);
    MultiGetOptions options;
    options.read_options.snapshot = &*snapshot;
    const auto moved = database_->MultiGet({}, options);
    ASSERT_FALSE(moved.has_value());
    EXPECT_EQ(moved.error().code(), ErrorCode::InvalidArgument);
    auto other = Database::Open(Creating(), root_ / "second");
    ASSERT_TRUE(other.has_value());
    options.read_options.snapshot = &retained;
    const auto foreign = other->MultiGet({}, options);
    ASSERT_FALSE(foreign.has_value());
    EXPECT_EQ(foreign.error().code(), ErrorCode::InvalidArgument);
    EXPECT_EQ(other->GetState().value().snapshot_count, 0U);
    EXPECT_EQ(database_->GetState().value().snapshot_count, 1U);
}

TEST_F(PublicMultiGetTest, MovedDatabaseIsAnOuterErrorRatherThanAnEmptySuccess) {
    Database retained = std::move(*database_);
    const auto rejected = database_->MultiGet({});
    ASSERT_FALSE(rejected.has_value());
    EXPECT_EQ(rejected.error().code(), ErrorCode::InvalidArgument);
    const auto empty = retained.MultiGet({});
    ASSERT_TRUE(empty.has_value());
    EXPECT_TRUE(empty->empty());
}

}  // namespace
}  // namespace modern_leveldb
