#include "engine/flush.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "engine/table_cache.h"
#include "format/internal_key.h"
#include "memory/memtable.h"
#include "metadata/version.h"
#include "metadata/version_edit.h"
#include "modern_leveldb/base/bytes.h"
#include "modern_leveldb/base/comparator.h"
#include "modern_leveldb/base/result.h"
#include "support/memory_file_system.h"

namespace modern_leveldb {
namespace {

using test_support::MemoryFileSystem;

static_assert(MaxMemTableOutputLevel == 2);

class ReverseComparator final : public Comparator {
 public:
  int Compare(ByteView left, ByteView right) const noexcept override {
    return BytewiseComparator().Compare(right, left);
  }
  std::string_view Name() const noexcept override { return "test.ReverseComparator"; }
  void FindShortestSeparator(std::vector<std::byte>&, ByteView) const override {}
  void FindShortSuccessor(std::vector<std::byte>&) const override {}
};

InternalKey Key(std::string_view user_key, SequenceNumber sequence, ValueKind kind) {
  return InternalKey::Create(AsBytes(user_key), sequence, kind).value();
}

// A table's metadata; the file itself is not needed to pick a level.
FileMetadata File(std::uint64_t number, std::string_view smallest, std::string_view largest,
                  std::uint64_t size = 100) {
  return FileMetadata{
      .number = number,
      .file_size = size,
      .smallest = Key(smallest, 100, ValueKind::Value),
      .largest = Key(largest, 1, ValueKind::Value),
  };
}

Version MakeVersion(const InternalKeyComparator& comparator,
                    std::initializer_list<std::pair<std::uint32_t, FileMetadata>> files) {
  VersionEdit edit;
  for (const auto& [level, file] : files) {
    EXPECT_TRUE(edit.AddFile(level, file).has_value());
  }
  VersionBuilder builder(comparator, Version());
  EXPECT_TRUE(builder.Apply(edit).has_value());
  Result<Version> version = builder.Build();
  EXPECT_TRUE(version.has_value());
  return version.has_value() ? std::move(*version) : Version();
}

class PickLevelTest : public testing::Test {
 protected:
  Version Make(std::initializer_list<std::pair<std::uint32_t, FileMetadata>> files) const {
    return MakeVersion(comparator_, files);
  }

  // Picks a level with a grandparent limit of ten times the target file size.
  static std::uint32_t Pick(const Version& version, std::string_view smallest,
                            std::string_view largest, std::uint64_t target_file_size = 100) {
    return PickLevelForMemTableOutput(version, BytewiseComparator(), AsBytes(smallest),
                                      AsBytes(largest), target_file_size);
  }

  InternalKeyComparator comparator_{BytewiseComparator()};
};

TEST_F(PickLevelTest, PushesATableWithNoOverlapsToLevelTwo) {
  EXPECT_EQ(Pick(Version(), "a", "z"), 2U);
  // Files below the grandparent levels do not matter.
  EXPECT_EQ(Pick(Make({{4, File(40, "a", "z", std::uint64_t{1} << 40U)}}), "b", "e"), 2U);
}

TEST_F(PickLevelTest, StaysAtLevelZeroWhenALevelZeroFileOverlaps) {
  // Level-0 files may overlap, so their largest keys need not be in order.
  const Version version = Make({{0, File(5, "a", "m")}, {0, File(6, "b", "c")}});

  EXPECT_EQ(Pick(version, "k", "l"), 0U);
  EXPECT_EQ(Pick(version, "0", "a"), 0U);
  EXPECT_EQ(Pick(version, "m", "z"), 0U);
  EXPECT_EQ(Pick(version, "bb", "bb"), 0U);
  EXPECT_EQ(Pick(version, "0", "z"), 0U);
  EXPECT_EQ(Pick(version, "n", "z"), 2U);
  EXPECT_EQ(Pick(version, "0", "1"), 2U);
}

TEST_F(PickLevelTest, StopsAboveALevelThatOverlaps) {
  const Version level1 =
      Make({{1, File(10, "c", "e")}, {1, File(11, "g", "i")}, {1, File(12, "m", "o")}});

  // Ranges that miss every file: before, between, and after them.
  EXPECT_EQ(Pick(level1, "a", "b"), 2U);
  EXPECT_EQ(Pick(level1, "f", "f"), 2U);
  EXPECT_EQ(Pick(level1, "j", "l"), 2U);
  EXPECT_EQ(Pick(level1, "p", "z"), 2U);
  // Ranges that touch, fall inside, span, or contain files.
  EXPECT_EQ(Pick(level1, "a", "c"), 0U);
  EXPECT_EQ(Pick(level1, "e", "f"), 0U);
  EXPECT_EQ(Pick(level1, "o", "z"), 0U);
  EXPECT_EQ(Pick(level1, "h", "h"), 0U);
  EXPECT_EQ(Pick(level1, "d", "n"), 0U);
  EXPECT_EQ(Pick(level1, "a", "z"), 0U);

  const Version level2 = Make({{2, File(20, "c", "e")}});
  EXPECT_EQ(Pick(level2, "d", "d"), 1U);
  EXPECT_EQ(Pick(level2, "f", "g"), 2U);
}

TEST_F(PickLevelTest, StopsAboveTooManyOverlappingGrandparentBytes) {
  // Level 2 holds the grandparents of a table at level 0, and the limit is
  // ten times the target file size.
  const Version over_level2 = Make({{2, File(20, "a", "c", 600)}, {2, File(21, "d", "f", 401)}});
  EXPECT_EQ(Pick(over_level2, "b", "e"), 0U);
  EXPECT_EQ(Pick(over_level2, "b", "e", 101), 1U);
  // A total at the limit does not stop the table; level 2 then overlaps it.
  EXPECT_EQ(Pick(Make({{2, File(20, "a", "c", 600)}, {2, File(21, "d", "f", 400)}}), "b", "e"), 1U);
  // Only files that overlap the range count.
  EXPECT_EQ(Pick(Make({{2, File(20, "a", "c", 600)}, {2, File(21, "x", "z", 5000)}}), "b", "e"),
            1U);

  // Level 3 holds the grandparents of a table at level 1.
  EXPECT_EQ(Pick(Make({{3, File(30, "a", "c", 600)}, {3, File(31, "d", "f", 401)}}), "b", "e"), 1U);
  EXPECT_EQ(Pick(Make({{3, File(30, "a", "c", 600)}, {3, File(31, "d", "f", 400)}}), "b", "e"), 2U);
  EXPECT_EQ(Pick(Make({{3, File(30, "a", "a", 5000)}, {3, File(31, "f", "z", 5000)}}), "b", "e"),
            2U);
}

TEST(PickLevelForMemTableOutputTest, ComparesUserKeysWithTheGivenComparator) {
  const ReverseComparator reverse;
  const InternalKeyComparator comparator(reverse);
  // In reverse order, "z" comes first.
  const Version version = MakeVersion(
      comparator, {{0, File(5, "f", "d")}, {1, File(10, "p", "n")}, {1, File(11, "k", "i")}});
  const auto pick = [&](std::string_view smallest, std::string_view largest) {
    return PickLevelForMemTableOutput(version, reverse, AsBytes(smallest), AsBytes(largest), 100);
  };

  EXPECT_EQ(pick("e", "e"), 0U);
  EXPECT_EQ(pick("o", "o"), 0U);
  EXPECT_EQ(pick("z", "a"), 0U);
  EXPECT_EQ(pick("m", "l"), 2U);
  EXPECT_EQ(pick("z", "q"), 2U);
  EXPECT_EQ(pick("c", "a"), 2U);
}

class FlushMemTableTest : public testing::Test {
 protected:
  FlushMemTableTest() {
    options_.target_file_size = 100;
    EXPECT_TRUE(memtable_.Add(3, ValueKind::Value, AsBytes("b"), AsBytes("beta")).has_value());
    EXPECT_TRUE(memtable_.Add(5, ValueKind::Deletion, AsBytes("a"), {}).has_value());
    EXPECT_TRUE(memtable_.Add(4, ValueKind::Value, AsBytes("c"), AsBytes("gamma")).has_value());
  }

  Result<VersionEdit> Flush(MemoryFileSystem& file_system, TableCache& cache,
                            const MemTable& memtable, const Version& base) const {
    return FlushMemTable(file_system, directory_, comparator_, options_, cache, memtable, 7, base,
                         9);
  }

  const std::filesystem::path directory_ = std::filesystem::path("db");
  const std::filesystem::path table_ = directory_ / "000007.ldb";
  InternalKeyComparator comparator_{BytewiseComparator()};
  FlushOptions options_;
  MemTable memtable_{BytewiseComparator()};
};

TEST_F(FlushMemTableTest, WritesTheTableAndReturnsTheEditThatInstallsIt) {
  MemoryFileSystem file_system;
  TableCache cache(file_system, directory_, comparator_, {}, 10);
  const Version base = MakeVersion(comparator_, {{1, File(20, "x", "z")}, {3, File(30, "a", "b")}});

  const Result<VersionEdit> edit = Flush(file_system, cache, memtable_, base);

  ASSERT_TRUE(edit.has_value()) << edit.error().ToString();
  ASSERT_EQ(edit->new_files().size(), 1U);
  const NewFile& added = edit->new_files()[0];
  EXPECT_EQ(added.level, 2U);
  EXPECT_EQ(added.file.number, 7U);
  const auto contents = file_system.Contents(table_);
  ASSERT_TRUE(contents.has_value());
  EXPECT_EQ(added.file.file_size, contents->size());
  EXPECT_EQ(comparator_.Compare(added.file.smallest.encoded(),
                                Key("a", 5, ValueKind::Deletion).encoded()),
            0);
  EXPECT_EQ(
      comparator_.Compare(added.file.largest.encoded(), Key("c", 4, ValueKind::Value).encoded()),
      0);
  EXPECT_EQ(edit->log_number(), 9U);
  EXPECT_EQ(edit->prev_log_number(), 0U);
  EXPECT_TRUE(edit->deleted_files().empty());
  EXPECT_TRUE(edit->compact_pointers().empty());
  EXPECT_FALSE(edit->comparator_name().has_value());
  EXPECT_FALSE(edit->next_file_number().has_value());
  EXPECT_FALSE(edit->last_sequence().has_value());

  // The directory is synced once, after the table is complete and verified.
  const std::vector<std::string>& operations = file_system.operations();
  ASSERT_FALSE(operations.empty());
  EXPECT_EQ(operations.back(), "sync_directory db");
  EXPECT_EQ(std::ranges::count(operations, "sync_directory db"), 1);
  EXPECT_NE(std::ranges::find(operations, "open_random_access 000007.ldb"), operations.end());

  // The table is cached.
  const std::size_t before = operations.size();
  ASSERT_TRUE(cache.Find(7, added.file.file_size).has_value());
  EXPECT_EQ(file_system.operations().size(), before);
}

TEST_F(FlushMemTableTest, PicksTheLevelInTheBaseVersion) {
  for (const auto& [base, level] : std::vector<std::pair<Version, std::uint32_t>>{
           {MakeVersion(comparator_, {{0, File(20, "b", "b")}}), 0},
           {MakeVersion(comparator_, {{2, File(20, "c", "d")}}), 1},
           {MakeVersion(comparator_, {{2, File(20, "a", "z", 1001)}}), 0}}) {
    SCOPED_TRACE(level);
    MemoryFileSystem file_system;
    TableCache cache(file_system, directory_, comparator_, {}, 10);

    const Result<VersionEdit> edit = Flush(file_system, cache, memtable_, base);

    ASSERT_TRUE(edit.has_value()) << edit.error().ToString();
    ASSERT_EQ(edit->new_files().size(), 1U);
    EXPECT_EQ(edit->new_files()[0].level, level);
  }
}

TEST_F(FlushMemTableTest, ReturnsAnEditWithoutFilesForAnEmptyMemtable) {
  MemoryFileSystem file_system;
  TableCache cache(file_system, directory_, comparator_, {}, 10);
  const MemTable empty(BytewiseComparator());

  const Result<VersionEdit> edit = Flush(file_system, cache, empty, Version());

  ASSERT_TRUE(edit.has_value()) << edit.error().ToString();
  EXPECT_TRUE(edit->new_files().empty());
  EXPECT_EQ(edit->log_number(), 9U);
  EXPECT_EQ(edit->prev_log_number(), 0U);
  EXPECT_TRUE(file_system.operations().empty());
}

TEST_F(FlushMemTableTest, ReturnsTheErrorOfAFailureAtEveryFileOperation) {
  MemoryFileSystem reference;
  TableCache reference_cache(reference, directory_, comparator_, {}, 10);
  ASSERT_TRUE(Flush(reference, reference_cache, memtable_, Version()).has_value());
  const std::vector<std::string> operations = reference.operations();

  for (std::size_t failing = 0; failing < operations.size(); ++failing) {
    SCOPED_TRACE(operations[failing]);
    MemoryFileSystem file_system;
    TableCache cache(file_system, directory_, comparator_, {}, 10);
    file_system.FailOperation(failing, Error::Io("injected failure"));

    const Result<VersionEdit> edit = Flush(file_system, cache, memtable_, Version());

    ASSERT_FALSE(edit.has_value());
    EXPECT_EQ(edit.error().message(), "injected failure");
    // A table that fails to build is removed; one whose directory sync fails
    // stays for obsolete-file cleanup.
    EXPECT_EQ(file_system.Contents(table_).has_value(), operations[failing] == "sync_directory db");
  }
}

}  // namespace
}  // namespace modern_leveldb
