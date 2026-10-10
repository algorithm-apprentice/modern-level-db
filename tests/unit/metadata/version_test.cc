#include "metadata/version.h"

#include <gtest/gtest.h>

#include <cstdint>
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
    const Version base = Applied(Version(), {Adding(1, metadata)});
    VersionEdit edit;
    ASSERT_TRUE(edit.RemoveFile(1, 1024).has_value());
    ASSERT_TRUE(edit.AddFile(1, File(2049, "key11024b", "key11024c")).has_value());

    const Version version = VersionBuilder::BuildTrusted(comparator_, base, edit);

    ASSERT_EQ(version.files(1).size(), 2048U);
    EXPECT_EQ(version.files(1).front(), base.files(1).front());
    EXPECT_EQ(version.files(1).back(), base.files(1).back());
    EXPECT_EQ(Numbers(version, 1)[1023], 2049U);
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
