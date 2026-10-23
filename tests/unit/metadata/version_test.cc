#include "metadata/version.h"

#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <type_traits>
#include <vector>

#include "format/internal_key.h"
#include "metadata/version_edit.h"
#include "modern_leveldb/base/bytes.h"
#include "modern_leveldb/base/comparator.h"
#include "modern_leveldb/base/result.h"

namespace modern_leveldb {
namespace {

static_assert(
    std::is_constructible_v<VersionBuilder, const InternalKeyComparator&, const Version&>);
static_assert(!std::is_constructible_v<VersionBuilder, InternalKeyComparator, const Version&>);

InternalKey Key(std::string_view user_key, SequenceNumber sequence) {
    auto key = InternalKey::Create(AsBytes(user_key), sequence, ValueKind::Value);
    EXPECT_TRUE(key.has_value());
    return std::move(key).value();
}

// A file spanning smallest@100 to largest@100.
FileMetadata File(std::uint64_t number, std::string_view smallest, std::string_view largest) {
    return {.number = number,
            .file_size = number * 1000,
            .smallest = Key(smallest, 100),
            .largest = Key(largest, 100)};
}

VersionEdit Adding(std::uint32_t level, const std::vector<FileMetadata>& files) {
    VersionEdit edit;
    for (const FileMetadata& file : files) {
        EXPECT_TRUE(edit.AddFile(level, file).has_value());
    }
    return edit;
}

std::vector<std::uint64_t> Numbers(const Version& version, std::uint32_t level) {
    std::vector<std::uint64_t> numbers;
    for (const Version::File& file : version.files(level)) {
        numbers.push_back(file->number);
    }
    return numbers;
}

class CountingComparator final : public Comparator {
public:
    int Compare(const ByteView& left, const ByteView& right) const noexcept override {
        comparisons_.fetch_add(1, std::memory_order_relaxed);
        return BytewiseComparator().Compare(left, right);
    }
    std::string_view Name() const noexcept override { return BytewiseComparator().Name(); }
    void FindShortestSeparator(std::vector<std::byte>& start, ByteView limit) const override {
        BytewiseComparator().FindShortestSeparator(start, limit);
    }
    void FindShortSuccessor(std::vector<std::byte>& key) const override {
        BytewiseComparator().FindShortSuccessor(key);
    }
    void Reset() noexcept { comparisons_.store(0, std::memory_order_relaxed); }
    std::size_t comparisons() const noexcept {
        return comparisons_.load(std::memory_order_relaxed);
    }

private:
    mutable std::atomic<std::size_t> comparisons_{0};
};

class VersionBuilderTest : public testing::Test {
protected:
    Result<Version> Apply(const Version& base, const std::vector<VersionEdit>& edits) {
        VersionBuilder builder(comparator_, base);
        for (const VersionEdit& edit : edits) {
            const Status applied = builder.Apply(edit);
            if (!applied.has_value()) {
                return std::unexpected(applied.error());
            }
        }
        return builder.Build();
    }

    Version Applied(const Version& base, const std::vector<VersionEdit>& edits) {
        auto version = Apply(base, edits);
        EXPECT_TRUE(version.has_value()) << version.error().ToString();
        return version.has_value() ? std::move(*version) : Version();
    }

    void ExpectRejected(const Version& base, const std::vector<VersionEdit>& edits) {
        const auto version = Apply(base, edits);
        ASSERT_FALSE(version.has_value());
        EXPECT_EQ(version.error().code(), ErrorCode::InvalidArgument);
    }

    InternalKeyComparator comparator_{BytewiseComparator()};
};

TEST(VersionTest, StartsWithoutFiles) {
    const Version version;
    for (std::uint32_t level = 0; level < NumLevels; ++level) {
        EXPECT_TRUE(version.files(level).empty());
    }
}

TEST_F(VersionBuilderTest, AddsFilesInKeyOrder) {
    const Version version =
        Applied(Version(), {Adding(0, {File(7, "m", "z"), File(5, "a", "p"), File(6, "m", "n")}),
                            Adding(1, {File(9, "k", "l"), File(8, "a", "c")})});

    EXPECT_EQ(Numbers(version, 0), (std::vector<std::uint64_t>{5, 6, 7}));
    EXPECT_EQ(Numbers(version, 1), (std::vector<std::uint64_t>{8, 9}));
    EXPECT_TRUE(version.files(2).empty());
    const FileMetadata& file = *version.files(1)[0];
    EXPECT_EQ(file.file_size, 8000U);
    EXPECT_EQ(comparator_.Compare(file.smallest, Key("a", 100)), 0);
    EXPECT_EQ(comparator_.Compare(file.largest, Key("c", 100)), 0);
}

TEST_F(VersionBuilderTest, DeletesAndMovesFilesAndSharesTheRest) {
    const Version base =
        Applied(Version(), {Adding(1, {File(1, "a", "b"), File(2, "c", "d"), File(3, "e", "f")})});
    VersionEdit edit = Adding(2, {File(2, "c", "d")});
    ASSERT_TRUE(edit.RemoveFile(1, 2).has_value());
    ASSERT_TRUE(edit.RemoveFile(1, 3).has_value());

    const Version version = Applied(base, {edit});

    EXPECT_EQ(Numbers(version, 1), (std::vector<std::uint64_t>{1}));
    EXPECT_EQ(Numbers(version, 2), (std::vector<std::uint64_t>{2}));
    EXPECT_EQ(version.files(1)[0], base.files(1)[0]);
    EXPECT_EQ(Numbers(base, 1), (std::vector<std::uint64_t>{1, 2, 3}));
}

TEST_F(VersionBuilderTest, TrustedBuildMatchesTheCheckedBuilder) {
    const Version base = Applied(Version(), {Adding(0, {File(1, "a", "z"), File(2, "b", "c")}),
                                             Adding(1, {File(3, "a", "b"), File(4, "d", "e")}),
                                             Adding(2, {File(5, "m", "n")})});
    VersionEdit edit = Adding(1, {File(7, "x", "z"), File(6, "g", "h")});
    ASSERT_TRUE(edit.RemoveFile(0, 2).has_value());
    ASSERT_TRUE(edit.RemoveFile(1, 3).has_value());
    ASSERT_TRUE(edit.RemoveFile(2, 5).has_value());
    ASSERT_TRUE(edit.AddFile(0, File(8, "a", "a")).has_value());
    ASSERT_TRUE(edit.AddFile(1, File(3, "b", "c")).has_value());
    ASSERT_TRUE(edit.AddFile(3, File(5, "m", "n")).has_value());

    const Version checked = Applied(base, {edit});
    const Version trusted = VersionBuilder::BuildTrusted(comparator_, base, edit);

    for (std::uint32_t level = 0; level < NumLevels; ++level) {
        ASSERT_EQ(trusted.files(level).size(), checked.files(level).size());
        for (std::size_t index = 0; index < checked.files(level).size(); ++index) {
            const FileMetadata& expected = *checked.files(level)[index];
            const FileMetadata& actual = *trusted.files(level)[index];
            EXPECT_EQ(actual.number, expected.number);
            EXPECT_EQ(actual.file_size, expected.file_size);
            EXPECT_EQ(comparator_.Compare(actual.smallest, expected.smallest), 0);
            EXPECT_EQ(comparator_.Compare(actual.largest, expected.largest), 0);
            EXPECT_EQ(actual.allowed_seeks, expected.allowed_seeks);
        }
    }
    EXPECT_EQ(trusted.files(0).front(), base.files(0).front());
    EXPECT_EQ(trusted.files(1)[1], base.files(1)[1]);
}

TEST_F(VersionBuilderTest, TrustedBuildLinearlyMergesALargeBase) {
    std::vector<FileMetadata> metadata;
    metadata.reserve(2048);
    for (std::uint64_t number = 1; number <= 2048; ++number) {
        const std::string prefix = "key" + std::to_string(10000 + number);
        metadata.push_back(File(number, prefix + "a", prefix + "z"));
    }
    CountingComparator user_comparator;
    const InternalKeyComparator comparator(user_comparator);
    VersionBuilder builder(comparator, Version());
    ASSERT_TRUE(builder.Apply(Adding(1, metadata)).has_value());
    Result<Version> built = builder.Build();
    ASSERT_TRUE(built.has_value());
    const Version base = std::move(*built);
    VersionEdit edit;
    ASSERT_TRUE(edit.RemoveFile(1, 1024).has_value());
    ASSERT_TRUE(edit.AddFile(1, File(2049, "key11024b", "key11024c")).has_value());

    user_comparator.Reset();
    const Version version = VersionBuilder::BuildTrusted(comparator, base, edit);
    EXPECT_LE(user_comparator.comparisons(), base.files(1).size() + 1);

    ASSERT_EQ(version.files(1).size(), 2048U);
    EXPECT_EQ(version.files(1).front(), base.files(1).front());
    EXPECT_EQ(version.files(1).back(), base.files(1).back());
    EXPECT_EQ(Numbers(version, 1)[1023], 2049U);
    for (std::size_t index = 0; index < version.files(1).size(); ++index) {
        if (index == 1023) {
            EXPECT_EQ(version.files(1)[index]->number, 2049U);
        } else {
            EXPECT_EQ(version.files(1)[index], base.files(1)[index]);
        }
    }
}

TEST_F(VersionBuilderTest, TrustedBuildPreservesTiesReplacementsAndEmptyDeltas) {
    const Version base =
        Applied(Version(), {Adding(0, {File(4, "a", "d"), File(2, "a", "c"), File(8, "z", "z")})});
    base.files(0)[1]->allowed_seeks = 17;
    const std::vector<Version::File> original(base.files(0).begin(), base.files(0).end());
    VersionEdit empty;
    VersionEdit deletions;
    ASSERT_TRUE(deletions.RemoveFile(0, 2).has_value());
    ASSERT_TRUE(deletions.RemoveFile(0, 8).has_value());
    VersionEdit additions = Adding(0, {File(9, "zz", "zz"), File(3, "a", "b")});
    VersionEdit replacement = Adding(0, {File(2, "a", "b"), File(1, "a", "b")});
    ASSERT_TRUE(replacement.RemoveFile(0, 2).has_value());
    ASSERT_TRUE(replacement.RemoveFile(0, 8).has_value());

    const std::vector<std::vector<std::uint64_t>> expected{
        {2, 4, 8}, {4}, {2, 3, 4, 8, 9}, {1, 2, 4}};
    std::size_t scenario = 0;
    for (const VersionEdit& edit : {empty, deletions, additions, replacement}) {
        SCOPED_TRACE(scenario);
        const std::vector<std::byte> encoded = edit.Encode();
        const Version checked = Applied(base, {edit});
        const Version trusted = VersionBuilder::BuildTrusted(comparator_, base, edit);
        EXPECT_EQ(Numbers(checked, 0), expected[scenario]);
        EXPECT_EQ(Numbers(trusted, 0), expected[scenario]);
        for (std::uint32_t level = 1; level < NumLevels; ++level) {
            EXPECT_TRUE(checked.files(level).empty());
            EXPECT_TRUE(trusted.files(level).empty());
        }
        for (const Version::File& file : trusted.files(0)) {
            for (const Version::File& old : original) {
                if (file->number == old->number && !edit.deleted_files().contains(DeletedFile{
                                                       .level = 0, .number = old->number})) {
                    EXPECT_EQ(file, old);
                    EXPECT_EQ(file->allowed_seeks, old->allowed_seeks);
                }
            }
        }
        if (scenario == 3) {
            EXPECT_NE(trusted.files(0)[1], original[0]);
            EXPECT_EQ(comparator_.Compare(trusted.files(0)[1]->largest, Key("b", 100)), 0);
        }
        EXPECT_EQ(edit.Encode(), encoded);
        EXPECT_EQ(std::vector<Version::File>(base.files(0).begin(), base.files(0).end()), original);
        EXPECT_EQ(Numbers(base, 0), (std::vector<std::uint64_t>{2, 4, 8}));
        EXPECT_EQ(base.files(0)[1]->allowed_seeks, 17);
        ++scenario;
    }

    const Version trusted_empty = VersionBuilder::BuildTrusted(comparator_, Version(), empty);
    for (std::uint32_t level = 0; level < NumLevels; ++level) {
        EXPECT_TRUE(trusted_empty.files(level).empty());
    }
}

TEST_F(VersionBuilderTest, InitializesNewSeekBudgetsWithoutChangingRetainedFilesOrEdits) {
    const Version base = Applied(Version(), {Adding(0, {File(1, "a", "b")})});
    base.files(0).front()->allowed_seeks = 7;
    constexpr std::uint64_t BytesPerSeek = 16384;
    for (const std::uint64_t size :
         {std::uint64_t{0}, 100 * BytesPerSeek - 1, 100 * BytesPerSeek, 101 * BytesPerSeek,
          std::numeric_limits<std::uint64_t>::max()}) {
        SCOPED_TRACE(size);
        FileMetadata file = File(2, "c", "d");
        file.file_size = size;
        file.allowed_seeks = 900;
        const VersionEdit edit = Adding(0, {file});
        const std::vector<std::byte> encoded = edit.Encode();
        const Version checked = Applied(base, {edit});
        const Version trusted = VersionBuilder::BuildTrusted(comparator_, base, edit);
        const std::int64_t expected =
            size / BytesPerSeek < 100 ? 100 : static_cast<std::int64_t>(size / BytesPerSeek);
        EXPECT_EQ(checked.files(0)[1]->allowed_seeks, expected);
        EXPECT_EQ(trusted.files(0)[1]->allowed_seeks, expected);
        EXPECT_EQ(trusted.files(0)[1]->file_size, size);
        EXPECT_EQ(checked.files(0).front(), base.files(0).front());
        EXPECT_EQ(trusted.files(0).front(), base.files(0).front());
        EXPECT_EQ(base.files(0).front()->allowed_seeks, 7);
        EXPECT_EQ(edit.new_files().front().file.allowed_seeks, 900);
        EXPECT_EQ(edit.Encode(), encoded);
        EXPECT_EQ(file.allowed_seeks, 900);
    }
}

TEST_F(VersionBuilderTest, ReportsTheFirstFailureInPhaseAndEntryOrder) {
    const Version base = Applied(Version(), {Adding(0, {File(1, "a", "b")})});
    VersionEdit missing_deletion = Adding(0, {File(1, "z", "a")});
    ASSERT_TRUE(missing_deletion.RemoveFile(0, 9).has_value());
    const VersionEdit live_reversed = Adding(0, {File(1, "z", "a")});
    const VersionEdit reversed_before_live = Adding(0, {File(2, "z", "a"), File(1, "a", "b")});
    const std::vector<std::string> messages{"edit deletes file 9, which is not live in level 0",
                                            "edit adds file 1, which is already live",
                                            "file 2 has its smallest key after its largest"};
    std::size_t scenario = 0;
    for (const VersionEdit& edit : {missing_deletion, live_reversed, reversed_before_live}) {
        SCOPED_TRACE(scenario);
        VersionBuilder builder(comparator_, base);
        const Status applied = builder.Apply(edit);
        ASSERT_FALSE(applied.has_value());
        EXPECT_EQ(applied.error().code(), ErrorCode::InvalidArgument);
        EXPECT_EQ(applied.error().message(), messages[scenario]);
        ++scenario;
    }
}

TEST_F(VersionBuilderTest, AppliesEditsInOrder) {
    VersionEdit removal;
    ASSERT_TRUE(removal.RemoveFile(0, 4).has_value());

    const Version version = Applied(
        Version(), {Adding(0, {File(4, "a", "b")}), removal, Adding(0, {File(4, "c", "d")})});

    ASSERT_EQ(Numbers(version, 0), (std::vector<std::uint64_t>{4}));
    EXPECT_EQ(comparator_.Compare(version.files(0)[0]->smallest, Key("c", 100)), 0);
}

TEST_F(VersionBuilderTest, RejectsDeletingFilesThatAreNotLive) {
    const Version base = Applied(Version(), {Adding(1, {File(1, "a", "b")})});
    for (const std::uint32_t level : {0U, 1U}) {
        VersionEdit edit;
        ASSERT_TRUE(edit.RemoveFile(level, level == 0 ? 1 : 2).has_value());
        ExpectRejected(base, {edit});
    }
}

TEST_F(VersionBuilderTest, RejectsAddingLiveFileNumbers) {
    const Version base = Applied(Version(), {Adding(1, {File(1, "a", "b")})});
    ExpectRejected(base, {Adding(1, {File(1, "x", "y")})});
    ExpectRejected(base, {Adding(3, {File(1, "a", "b")})});
    ExpectRejected(Version(), {Adding(0, {File(2, "a", "b"), File(2, "c", "d")})});
}

TEST_F(VersionBuilderTest, RejectsFilesWhoseSmallestKeyFollowsTheLargest) {
    ExpectRejected(Version(), {Adding(0, {File(1, "b", "a")})});
    FileMetadata newer_after_older = File(1, "a", "a");
    newer_after_older.smallest = Key("a", 5);
    newer_after_older.largest = Key("a", 6);
    ExpectRejected(Version(), {Adding(0, {newer_after_older})});

    const Version version = Applied(Version(), {Adding(0, {File(1, "a", "a")})});
    EXPECT_EQ(Numbers(version, 0), (std::vector<std::uint64_t>{1}));
}

TEST_F(VersionBuilderTest, RejectsOverlappingFilesAboveLevelZero) {
    ExpectRejected(Version(), {Adding(1, {File(1, "a", "m"), File(2, "k", "z")})});
    // Files may not share a boundary key.
    ExpectRejected(Version(), {Adding(2, {File(1, "a", "k"), File(2, "k", "z")})});
    const Version base = Applied(Version(), {Adding(6, {File(1, "a", "k")})});
    ExpectRejected(base, {Adding(6, {File(2, "j", "z")})});

    // Different versions of one user key may end one file and start the next.
    FileMetadata first = File(1, "a", "k");
    FileMetadata second = File(2, "k", "z");
    first.largest = Key("k", 9);
    second.smallest = Key("k", 8);
    const Version version =
        Applied(Version(), {Adding(1, {first, second}), Adding(0, {File(3, "a", "z")}),
                            Adding(0, {File(4, "b", "c")})});
    EXPECT_EQ(Numbers(version, 1), (std::vector<std::uint64_t>{1, 2}));
    EXPECT_EQ(Numbers(version, 0), (std::vector<std::uint64_t>{3, 4}));
}

}  // namespace
}  // namespace modern_leveldb
