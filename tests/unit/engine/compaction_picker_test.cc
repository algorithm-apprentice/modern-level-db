#include "engine/compaction_picker.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <initializer_list>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string_view>
#include <utility>
#include <vector>

#include "format/internal_key.h"
#include "metadata/version.h"
#include "metadata/version_edit.h"
#include "modern_leveldb/base/bytes.h"
#include "modern_leveldb/base/comparator.h"
#include "modern_leveldb/base/result.h"

namespace modern_leveldb {
namespace {

constexpr std::uint64_t MiB = std::uint64_t{1} << 20U;
// Expansions must stay below 25,000 bytes, and a trivial move allows 10,000
// grandparent bytes.
constexpr std::uint64_t TargetFileSize = 1000;

static_assert(Level0CompactionTrigger == 4);

class ReverseComparator final : public Comparator {
 public:
  int Compare(ByteView left, ByteView right) const noexcept override {
    return BytewiseComparator().Compare(right, left);
  }
  std::string_view Name() const noexcept override { return "test.ReverseComparator"; }
  void FindShortestSeparator(std::vector<std::byte>&, ByteView) const override {}
  void FindShortSuccessor(std::vector<std::byte>&) const override {}
};

InternalKey Key(std::string_view user_key, SequenceNumber sequence) {
  return InternalKey::Create(AsBytes(user_key), sequence, ValueKind::Value).value();
}

FileMetadata File(std::uint64_t number, InternalKey smallest, InternalKey largest,
                  std::uint64_t size = 100) {
  return FileMetadata{.number = number,
                      .file_size = size,
                      .smallest = std::move(smallest),
                      .largest = std::move(largest)};
}

// A file whose smallest key has sequence 100 and whose largest has sequence 1.
FileMetadata File(std::uint64_t number, std::string_view smallest, std::string_view largest,
                  std::uint64_t size = 100) {
  return File(number, Key(smallest, 100), Key(largest, 1), size);
}

std::shared_ptr<const Version> MakeVersion(
    const InternalKeyComparator& comparator,
    std::initializer_list<std::pair<std::uint32_t, FileMetadata>> files) {
  VersionEdit edit;
  for (const auto& [level, file] : files) {
    EXPECT_TRUE(edit.AddFile(level, file).has_value());
  }
  VersionBuilder builder(comparator, Version());
  EXPECT_TRUE(builder.Apply(edit).has_value());
  Result<Version> version = builder.Build();
  EXPECT_TRUE(version.has_value());
  return std::make_shared<const Version>(version.has_value() ? std::move(*version) : Version());
}

std::vector<std::uint64_t> Numbers(std::span<const Version::File> files) {
  std::vector<std::uint64_t> numbers;
  for (const Version::File& file : files) {
    numbers.push_back(file->number);
  }
  return numbers;
}

using Numbered = std::vector<std::uint64_t>;

class CompactionPickerTest : public testing::Test {
 protected:
  std::shared_ptr<const Version> Make(
      std::initializer_list<std::pair<std::uint32_t, FileMetadata>> files) const {
    return MakeVersion(comparator_, files);
  }

  std::optional<Compaction> Pick(const std::shared_ptr<const Version>& version,
                                 const std::optional<SeekCompaction>& seek = std::nullopt) const {
    return PickCompaction(version, comparator_, pointers_, seek, TargetFileSize);
  }

  static std::pair<std::uint32_t, double> Score(const std::shared_ptr<const Version>& version) {
    const CompactionScore score = ScoreCompaction(*version);
    return {score.level, score.score};
  }

  // Expects the compaction's level, its inputs in both levels, and its
  // grandparents, by file number.
  static void ExpectCompaction(const std::optional<Compaction>& compaction, std::uint32_t level,
                               const Numbered& inputs, const Numbered& next_inputs,
                               const Numbered& grandparents = {}) {
    ASSERT_TRUE(compaction.has_value());
    EXPECT_EQ(compaction->level, level);
    EXPECT_EQ(Numbers(compaction->inputs[0]), inputs);
    EXPECT_EQ(Numbers(compaction->inputs[1]), next_inputs);
    EXPECT_EQ(Numbers(compaction->grandparents), grandparents);
  }

  void ExpectPointer(const std::optional<Compaction>& compaction, const InternalKey& key) const {
    ASSERT_TRUE(compaction.has_value());
    EXPECT_EQ(comparator_.Compare(compaction->compact_pointer, key), 0);
  }

  InternalKeyComparator comparator_{BytewiseComparator()};
  std::array<std::optional<InternalKey>, NumLevels> pointers_{};
};

TEST_F(CompactionPickerTest, ScoresLevelZeroByFileCountAndDeeperLevelsBySize) {
  EXPECT_EQ(Score(Make({})), (std::pair<std::uint32_t, double>{0, 0.0}));
  EXPECT_EQ(Score(Make({{0, File(1, "a", "b")}, {0, File(2, "a", "b")}, {0, File(3, "a", "b")}})),
            (std::pair<std::uint32_t, double>{0, 0.75}));
  EXPECT_EQ(Score(Make({{0, File(1, "a", "b")}, {1, File(2, "a", "b", 5 * MiB)}})),
            (std::pair<std::uint32_t, double>{1, 0.5}));
  EXPECT_EQ(Score(Make({{2, File(1, "a", "b", 150 * MiB)}})),
            (std::pair<std::uint32_t, double>{2, 1.5}));
  EXPECT_EQ(Score(Make({{5, File(1, "a", "b", 100000 * MiB)}})),
            (std::pair<std::uint32_t, double>{5, 1.0}));
  // The last level never scores.
  EXPECT_EQ(Score(Make({{6, File(1, "a", "b", 1000000 * MiB)}})),
            (std::pair<std::uint32_t, double>{0, 0.0}));
}

TEST_F(CompactionPickerTest, ScoresTiesForTheFirstLevel) {
  EXPECT_EQ(Score(Make({{0, File(1, "a", "b")},
                        {0, File(2, "a", "b")},
                        {0, File(3, "a", "b")},
                        {0, File(4, "a", "b")},
                        {1, File(5, "a", "b", 10 * MiB)}})),
            (std::pair<std::uint32_t, double>{0, 1.0}));
  EXPECT_EQ(Score(Make({{1, File(5, "a", "b", 10 * MiB)}, {2, File(6, "a", "b", 100 * MiB)}})),
            (std::pair<std::uint32_t, double>{1, 1.0}));
}

TEST_F(CompactionPickerTest, PicksNothingWithoutAFullLevelOrASeekCompaction) {
  EXPECT_FALSE(Pick(Make({})).has_value());
  EXPECT_FALSE(Pick(Make({{0, File(1, "a", "b")}, {1, File(2, "c", "d", 9 * MiB)}})).has_value());
}

TEST_F(CompactionPickerTest, StartsASizeCompactionAfterTheCompactPointer) {
  const auto version = Make({{1, File(10, "a", "c", 4 * MiB)},
                             {1, File(11, "d", "f", 4 * MiB)},
                             {1, File(12, "g", "i", 4 * MiB)},
                             {2, File(20, "b", "e")},
                             {2, File(21, "x", "z")}});

  const std::optional<Compaction> first = Pick(version);
  // Expanding to file 11 would exceed 25 times the target file size.
  ExpectCompaction(first, 1, {10}, {20});
  ExpectPointer(first, Key("c", 1));
  ASSERT_TRUE(first.has_value());
  EXPECT_EQ(first->version, version);

  pointers_[1] = Key("c", 1);
  ExpectCompaction(Pick(version), 1, {11}, {20});
  ExpectPointer(Pick(version), Key("f", 1));
  // The pointer compares internal keys: c@50 comes before c@1.
  pointers_[1] = Key("c", 50);
  ExpectCompaction(Pick(version), 1, {10}, {20});
  pointers_[1] = Key("f", 1);
  ExpectCompaction(Pick(version), 1, {12}, {});
  // Past the last file, the level starts over.
  pointers_[1] = Key("i", 1);
  ExpectCompaction(Pick(version), 1, {10}, {20});
  // Other levels' pointers do not matter.
  pointers_ = {};
  pointers_[2] = Key("z", 1);
  ExpectCompaction(Pick(version), 1, {10}, {20});
}

TEST_F(CompactionPickerTest, StartsASeekCompactionWhenNoLevelIsFull) {
  const auto version =
      Make({{1, File(10, "a", "c", MiB)}, {1, File(11, "d", "f", MiB)}, {2, File(20, "e", "e")}});
  const SeekCompaction seek{.level = 1, .file = version->files(1)[1]};

  const std::optional<Compaction> compaction = Pick(version, seek);

  ExpectCompaction(compaction, 1, {11}, {20});
  ExpectPointer(compaction, Key("f", 1));

  // A full level comes first.
  const auto full = Make({{0, File(1, "a", "b")},
                          {0, File(2, "a", "b")},
                          {0, File(3, "a", "b")},
                          {0, File(4, "a", "b")},
                          {1, File(10, "x", "z")}});
  ExpectCompaction(Pick(full, SeekCompaction{.level = 1, .file = full->files(1)[0]}), 0,
                   {1, 2, 3, 4}, {});
}

TEST_F(CompactionPickerTest, ExpandsLevelZeroInputsUntilTheirRangeStopsGrowing) {
  const auto version = Make({{0, File(1, "a", "c")},
                             {0, File(2, "c", "e")},
                             {0, File(3, "e", "g")},
                             {0, File(4, "x", "z")}});

  // File 1 reaches file 2, which reaches file 3.
  const std::optional<Compaction> first = Pick(version);
  ExpectCompaction(first, 0, {1, 2, 3}, {});
  ExpectPointer(first, Key("g", 1));
  // File 2 reaches back to file 1.
  pointers_[0] = Key("c", 1);
  ExpectCompaction(Pick(version), 0, {1, 2, 3}, {});
  // File 3 reaches file 1 only through file 2.
  pointers_[0] = Key("e", 1);
  ExpectCompaction(Pick(version), 0, {1, 2, 3}, {});
  pointers_[0] = Key("g", 1);
  ExpectCompaction(Pick(version), 0, {4}, {});

  // A seek compaction at level 0 expands too.
  const auto two = Make({{0, File(1, "a", "c")}, {0, File(2, "b", "d")}});
  ExpectCompaction(Pick(two, SeekCompaction{.level = 0, .file = two->files(0)[1]}), 0, {1, 2}, {});
}

TEST_F(CompactionPickerTest, AddsBoundaryFilesThatShareAUserKey) {
  // Files 21, 24, and 25 split the entries of user key "k".
  const auto level = Make({{1, File(21, Key("a", 9), Key("k", 5), 4 * MiB)},
                           {1, File(24, Key("k", 3), Key("k", 2), 4 * MiB)},
                           {1, File(25, Key("k", 1), Key("m", 1), 4 * MiB)},
                           {1, File(26, "n", "p")}});
  const std::optional<Compaction> compaction = Pick(level);
  ExpectCompaction(compaction, 1, {21, 24, 25}, {});
  ExpectPointer(compaction, Key("m", 1));

  // The next level's inputs get their boundary files too.
  const auto next = Make({{1, File(30, "a", "c", 11 * MiB)},
                          {2, File(40, Key("b", 9), Key("d", 5))},
                          {2, File(41, Key("d", 3), Key("f", 1))},
                          {2, File(42, "g", "h")}});
  ExpectCompaction(Pick(next), 1, {30}, {40, 41});
}

TEST_F(CompactionPickerTest, ExpandsLevelInputsThatKeepTheNextLevelInputs) {
  // File 59 fills the level without overlapping the others.
  const auto grow = [&](std::uint64_t size, std::uint64_t next_size) {
    return Make({{1, File(50, "a", "b", size)},
                 {1, File(51, "c", "d", size)},
                 {1, File(59, "x", "z", 10 * MiB)},
                 {2, File(60, "b", "c", next_size)}});
  };

  const std::optional<Compaction> expanded = Pick(grow(5000, 5000));
  ExpectCompaction(expanded, 1, {50, 51}, {60});
  ExpectPointer(expanded, Key("d", 1));
  ExpectCompaction(Pick(grow(10000, 4999)), 1, {50, 51}, {60});
  // The expanded inputs must total less than 25 times the target file size.
  const std::optional<Compaction> at_limit = Pick(grow(10000, 5000));
  ExpectCompaction(at_limit, 1, {50}, {60});
  ExpectPointer(at_limit, Key("b", 1));

  // Expanding must not add next-level inputs.
  ExpectCompaction(Pick(Make({{1, File(50, "a", "b")},
                              {1, File(51, "c", "d")},
                              {1, File(59, "x", "z", 10 * MiB)},
                              {2, File(60, "b", "c")},
                              {2, File(61, "d", "e")}})),
                   1, {50}, {60});
}

TEST_F(CompactionPickerTest, CollectsTheGrandparentsOfTheWholeRange) {
  ExpectCompaction(Pick(Make({{1, File(70, "c", "e", 11 * MiB)},
                              {2, File(71, "d", "g")},
                              {3, File(80, "a", "b")},
                              {3, File(81, "d", "d")},
                              {3, File(82, "f", "h")},
                              {3, File(83, "x", "y")}})),
                   1, {70}, {71}, {81, 82});

  // Level 4's grandparents are in level 6, and level 5 has none.
  const auto deep = Make({{4, File(90, "a", "b")},
                          {5, File(91, "a", "c")},
                          {6, File(95, "a", "a")},
                          {6, File(96, "z", "z")}});
  ExpectCompaction(Pick(deep, SeekCompaction{.level = 4, .file = deep->files(4)[0]}), 4, {90}, {91},
                   {95});
  ExpectCompaction(Pick(deep, SeekCompaction{.level = 5, .file = deep->files(5)[0]}), 5, {91}, {95},
                   {});
}

TEST_F(CompactionPickerTest, MovesOneFileWithFewGrandparentBytes) {
  const auto move = [&](std::uint64_t grandparent_size) {
    return Pick(
        Make({{1, File(70, "c", "e", 11 * MiB)}, {3, File(80, "d", "d", grandparent_size)}}));
  };

  const std::optional<Compaction> at_limit = move(10000);
  ASSERT_TRUE(at_limit.has_value());
  EXPECT_TRUE(IsTrivialMove(*at_limit, TargetFileSize));
  EXPECT_FALSE(IsTrivialMove(*at_limit, TargetFileSize - 1));
  const std::optional<Compaction> over = move(10001);
  ASSERT_TRUE(over.has_value());
  EXPECT_FALSE(IsTrivialMove(*over, TargetFileSize));

  // More than one input, or a next-level input, needs merging.
  const std::optional<Compaction> expanded = Pick(Make({{1, File(50, "a", "b")},
                                                        {1, File(51, "c", "d")},
                                                        {1, File(59, "x", "z", 10 * MiB)},
                                                        {2, File(60, "b", "c")}}));
  ASSERT_TRUE(expanded.has_value());
  EXPECT_FALSE(IsTrivialMove(*expanded, TargetFileSize));
  const std::optional<Compaction> merged =
      Pick(Make({{1, File(70, "c", "e", 11 * MiB)}, {2, File(71, "d", "g")}}));
  ASSERT_TRUE(merged.has_value());
  EXPECT_FALSE(IsTrivialMove(*merged, TargetFileSize));
}

TEST_F(CompactionPickerTest, EditsRemoveTheInputsAndRecordThePointer) {
  const std::optional<Compaction> compaction = Pick(Make({{1, File(50, "a", "b")},
                                                          {1, File(51, "c", "d")},
                                                          {1, File(59, "x", "z", 10 * MiB)},
                                                          {2, File(60, "b", "c")}}));
  ASSERT_TRUE(compaction.has_value());

  const VersionEdit edit = CompactionEdit(*compaction);

  EXPECT_EQ(edit.deleted_files(), (std::set<DeletedFile>{{.level = 1, .number = 50},
                                                         {.level = 1, .number = 51},
                                                         {.level = 2, .number = 60}}));
  ASSERT_EQ(edit.compact_pointers().size(), 1U);
  EXPECT_EQ(edit.compact_pointers()[0].level, 1U);
  EXPECT_EQ(comparator_.Compare(edit.compact_pointers()[0].key, Key("d", 1)), 0);
  EXPECT_TRUE(edit.new_files().empty());
  EXPECT_FALSE(edit.log_number().has_value());
  EXPECT_FALSE(edit.next_file_number().has_value());
}

TEST_F(CompactionPickerTest, AppliesATrivialMoveThroughItsEdit) {
  const auto version = Make({{1, File(70, "c", "e", 11 * MiB)}, {3, File(80, "a", "z")}});
  const std::optional<Compaction> compaction = Pick(version);
  ASSERT_TRUE(compaction.has_value());
  ASSERT_TRUE(IsTrivialMove(*compaction, TargetFileSize));

  VersionEdit edit = CompactionEdit(*compaction);
  ASSERT_TRUE(edit.AddFile(compaction->level + 1, *compaction->inputs[0][0]).has_value());
  VersionBuilder builder(comparator_, *version);
  ASSERT_TRUE(builder.Apply(edit).has_value());
  const Result<Version> moved = builder.Build();

  ASSERT_TRUE(moved.has_value());
  EXPECT_TRUE(moved->files(1).empty());
  EXPECT_EQ(Numbers(moved->files(2)), (Numbered{70}));
  EXPECT_EQ(Numbers(moved->files(3)), (Numbered{80}));
}

TEST(CompactionPickerReverseTest, OrdersKeysWithTheVersionsComparator) {
  const ReverseComparator reverse;
  const InternalKeyComparator comparator(reverse);
  // In reverse order, "z" comes first.
  const auto version = MakeVersion(comparator, {{0, File(1, "g", "e")},
                                                {0, File(2, "e", "c")},
                                                {0, File(3, "c", "a")},
                                                {0, File(4, "z", "x")}});
  std::array<std::optional<InternalKey>, NumLevels> pointers{};
  pointers[0] = Key("x", 1);

  const std::optional<Compaction> compaction =
      PickCompaction(version, comparator, pointers, std::nullopt, TargetFileSize);

  ASSERT_TRUE(compaction.has_value());
  EXPECT_EQ(Numbers(compaction->inputs[0]), (Numbered{1, 2, 3}));
  EXPECT_EQ(comparator.Compare(compaction->compact_pointer, Key("a", 1)), 0);
}

}  // namespace
}  // namespace modern_leveldb
