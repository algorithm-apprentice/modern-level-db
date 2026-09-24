#include "engine/iterators.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <memory>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "engine/build_table.h"
#include "engine/internal_iterator.h"
#include "engine/table_cache.h"
#include "format/internal_key.h"
#include "memory/memtable.h"
#include "metadata/filenames.h"
#include "metadata/version.h"
#include "metadata/version_edit.h"
#include "modern_leveldb/base/bytes.h"
#include "modern_leveldb/base/comparator.h"
#include "modern_leveldb/base/result.h"
#include "support/memory_file_system.h"
#include "support/scripted_iterator.h"
#include "table/table.h"
#include "table/table_builder.h"

namespace modern_leveldb {
namespace {

using test_support::MemoryFileSystem;
using test_support::MoveScript;
using test_support::ScriptedEntry;
using test_support::ScriptedIterator;

static_assert(!std::is_copy_constructible_v<InternalIterator>);
static_assert(!std::is_move_constructible_v<InternalIterator>);

template <typename T>
concept MergeableWith =
    requires(T&& comparator) { NewMergingIterator(std::forward<T>(comparator), {}); };
static_assert(MergeableWith<const InternalKeyComparator&>);
static_assert(!MergeableWith<InternalKeyComparator>);

template <typename T>
concept LevelWith =
    requires(std::shared_ptr<const Version> version, TableCache& cache, T&& comparator) {
      NewLevelIterator(version, version->files(1), cache, std::forward<T>(comparator),
                       TableReadOptions{});
    };
static_assert(LevelWith<const InternalKeyComparator&>);
static_assert(!LevelWith<InternalKeyComparator>);

template <typename T>
concept InternalWith =
    requires(std::shared_ptr<const MemTable> memtable, std::shared_ptr<const Version> version,
             TableCache& cache, T&& comparator) {
      NewInternalIterator(memtable, memtable, version, cache, std::forward<T>(comparator),
                          TableReadOptions{});
    };
static_assert(InternalWith<const InternalKeyComparator&>);
static_assert(!InternalWith<InternalKeyComparator>);

std::vector<std::byte> Bytes(std::string_view text) {
  return std::vector<std::byte>(AsBytes(text).begin(), AsBytes(text).end());
}

std::vector<std::byte> Key(std::string_view user_key, SequenceNumber sequence,
                           ValueKind kind = ValueKind::Value) {
  auto key = InternalKey::Create(AsBytes(user_key), sequence, kind);
  EXPECT_TRUE(key.has_value());
  return std::vector<std::byte>(key->encoded().begin(), key->encoded().end());
}

// "user@sequence=value" for the iterator's position, or "<end>".
std::string Describe(const InternalIterator& iterator) {
  if (!iterator.valid()) {
    return "<end>";
  }
  const ParsedInternalKey key = ParseInternalKey(iterator.key()).value();
  return std::string(AsStringView(key.user_key)) + "@" + std::to_string(key.sequence) + "=" +
         std::string(AsStringView(iterator.value()));
}

std::vector<std::string> ScanForward(InternalIterator& iterator) {
  std::vector<std::string> entries;
  Status status = iterator.SeekToFirst();
  while (status.has_value() && iterator.valid()) {
    entries.push_back(Describe(iterator));
    status = iterator.Next();
  }
  EXPECT_TRUE(status.has_value()) << status.error().ToString();
  return entries;
}

std::vector<std::string> ScanBackward(InternalIterator& iterator) {
  std::vector<std::string> entries;
  Status status = iterator.SeekToLast();
  while (status.has_value() && iterator.valid()) {
    entries.push_back(Describe(iterator));
    status = iterator.Prev();
  }
  EXPECT_TRUE(status.has_value()) << status.error().ToString();
  std::ranges::reverse(entries);
  return entries;
}

// Expects the move to succeed and describes the position after it.
std::string After(const Status& moved, const InternalIterator& iterator) {
  EXPECT_TRUE(moved.has_value()) << moved.error().ToString();
  return Describe(iterator);
}

ScriptedEntry Entry(std::string_view user_key, SequenceNumber sequence, std::string_view value) {
  return {.key = Key(user_key, sequence), .value = Bytes(value)};
}

class MergingIteratorTest : public testing::Test {
 protected:
  std::unique_ptr<InternalIterator> Merge(
      std::initializer_list<std::vector<ScriptedEntry>> children,
      const std::shared_ptr<MoveScript>& script = std::make_shared<MoveScript>()) {
    std::vector<std::unique_ptr<InternalIterator>> iterators;
    for (const std::vector<ScriptedEntry>& entries : children) {
      iterators.push_back(std::make_unique<ScriptedIterator>(comparator_, entries, script));
    }
    return NewMergingIterator(comparator_, std::move(iterators));
  }

  InternalKeyComparator comparator_{BytewiseComparator()};
};

TEST_F(MergingIteratorTest, MergesChildrenInOrder) {
  const auto merged = Merge({{Entry("a", 1, "a"), Entry("d", 1, "d"), Entry("g", 1, "g")},
                             {Entry("b", 1, "b"), Entry("e", 1, "e")},
                             {},
                             {Entry("c", 2, "c2"), Entry("c", 1, "c1"), Entry("f", 1, "f")}});
  const std::vector<std::string> expected{"a@1=a", "b@1=b", "c@2=c2", "c@1=c1",
                                          "d@1=d", "e@1=e", "f@1=f",  "g@1=g"};

  EXPECT_EQ(ScanForward(*merged), expected);
  EXPECT_EQ(ScanBackward(*merged), expected);
  EXPECT_EQ(After(merged->Seek(Key("c", 1)), *merged), "c@1=c1");
  EXPECT_EQ(After(merged->Seek(Key("c", 5)), *merged), "c@2=c2");
  EXPECT_EQ(After(merged->Seek(Key("dd", 5)), *merged), "e@1=e");
  EXPECT_EQ(After(merged->Seek(Key("h", 5)), *merged), "<end>");
}

TEST_F(MergingIteratorTest, HasNoEntriesWithoutChildrenOrEntries) {
  const auto none = NewMergingIterator(comparator_, {});
  EXPECT_EQ(After(none->SeekToFirst(), *none), "<end>");
  EXPECT_EQ(After(none->SeekToLast(), *none), "<end>");
  EXPECT_EQ(After(none->Seek(Key("a", 1)), *none), "<end>");

  const auto empty = Merge({{}, {}});
  EXPECT_TRUE(ScanForward(*empty).empty());
  EXPECT_TRUE(ScanBackward(*empty).empty());
}

TEST_F(MergingIteratorTest, OrdersEqualKeysByChild) {
  const auto merged = Merge({{Entry("a", 1, "first"), Entry("b", 1, "first")},
                             {Entry("b", 1, "second"), Entry("c", 1, "second")}});

  EXPECT_EQ(ScanForward(*merged),
            (std::vector<std::string>{"a@1=first", "b@1=first", "b@1=second", "c@1=second"}));
  // Moving backward yields the last child's entry first, so it retraces the
  // forward order.
  EXPECT_EQ(ScanBackward(*merged),
            (std::vector<std::string>{"a@1=first", "b@1=first", "b@1=second", "c@1=second"}));

  // Changing direction moves the other children past the current key, so
  // their equal entries are passed over, as in LevelDB.
  EXPECT_EQ(After(merged->SeekToLast(), *merged), "c@1=second");
  EXPECT_EQ(After(merged->Prev(), *merged), "b@1=second");
  EXPECT_EQ(After(merged->Prev(), *merged), "b@1=first");
  EXPECT_EQ(After(merged->Next(), *merged), "c@1=second");
  EXPECT_EQ(After(merged->Seek(Key("b", 1)), *merged), "b@1=first");
  EXPECT_EQ(After(merged->Next(), *merged), "b@1=second");
  EXPECT_EQ(After(merged->Prev(), *merged), "a@1=first");
}

TEST_F(MergingIteratorTest, ChangesDirectionAtEveryPosition) {
  std::mt19937_64 random(0x4e7'2026);
  const auto below = [&](std::uint64_t bound) {
    return std::uniform_int_distribution<std::uint64_t>(0, bound - 1)(random);
  };
  for (int round = 0; round < 20; ++round) {
    SCOPED_TRACE(round);
    // Unique keys spread over the children.
    std::vector<std::vector<ScriptedEntry>> children(1 + below(5));
    // Compares numbers rather than encoded keys, whose comparison GCC's
    // -Wstringop-overread misreads at -O3.
    std::set<std::pair<std::uint64_t, SequenceNumber>> unique;
    std::vector<std::vector<std::byte>> keys;
    for (std::uint64_t count = below(40); count > 0; --count) {
      const std::uint64_t key_number = below(30);
      const SequenceNumber sequence = 1 + below(20);
      if (!unique.emplace(key_number, sequence).second) {
        continue;
      }
      const std::string user_key = "k" + std::to_string(key_number);
      keys.push_back(Key(user_key, sequence));
      children[below(children.size())].push_back(Entry(user_key, sequence, user_key));
    }
    const auto less = [this](const auto& left, const auto& right) {
      return comparator_.Compare(left, right) < 0;
    };
    std::ranges::sort(keys, less);
    std::vector<std::unique_ptr<InternalIterator>> iterators;
    for (const std::vector<ScriptedEntry>& entries : children) {
      iterators.push_back(std::make_unique<ScriptedIterator>(comparator_, entries));
    }
    const auto merged = NewMergingIterator(comparator_, std::move(iterators));
    const std::vector<std::string> model = ScanForward(*merged);
    ASSERT_EQ(model.size(), keys.size());
    ASSERT_EQ(ScanBackward(*merged), model);

    // A random walk that changes direction often.
    std::optional<std::size_t> position;
    for (int step = 0; step < 300; ++step) {
      const std::uint64_t operation = below(position.has_value() ? 5 : 3);
      Status moved;
      if (operation == 0) {
        moved = merged->SeekToFirst();
        position = model.empty() ? std::nullopt : std::optional<std::size_t>(0);
      } else if (operation == 1) {
        moved = merged->SeekToLast();
        position = model.empty() ? std::nullopt : std::optional(model.size() - 1);
      } else if (operation == 2) {
        const std::string user_key = "k" + std::to_string(below(32));
        const std::vector<std::byte> target = Key(user_key, 1 + below(22));
        moved = merged->Seek(target);
        const auto found = std::ranges::lower_bound(keys, target, less);
        position = found == keys.end()
                       ? std::nullopt
                       : std::optional<std::size_t>(std::distance(keys.begin(), found));
      } else if (operation == 3) {
        moved = merged->Next();
        position = *position + 1 == model.size() ? std::nullopt : std::optional(*position + 1);
      } else {
        moved = merged->Prev();
        position = *position == 0 ? std::nullopt : std::optional(*position - 1);
      }
      ASSERT_TRUE(moved.has_value());
      ASSERT_EQ(Describe(*merged), position.has_value() ? model[*position] : "<end>");
    }
  }
}

TEST_F(MergingIteratorTest, FailsWhilePassingAnEqualKey) {
  const auto script = std::make_shared<MoveScript>();
  const auto merged = Merge({{Entry("b", 1, "first")}, {Entry("b", 1, "second")}}, script);
  EXPECT_EQ(After(merged->SeekToLast(), *merged), "b@1=second");
  EXPECT_EQ(After(merged->Prev(), *merged), "b@1=first");
  // Changing direction seeks the other child to the key and then moves it past
  // its equal entry, which fails.
  script->fail_at = script->moves + 2;

  const Status failed = merged->Next();

  ASSERT_FALSE(failed.has_value());
  EXPECT_EQ(failed.error().message(), "injected failure");
  EXPECT_FALSE(merged->valid());
}

TEST_F(MergingIteratorTest, FailsWithAnyChildMove) {
  const auto run = [&](const std::shared_ptr<MoveScript>& script) {
    const auto merged = Merge({{Entry("a", 1, "a"), Entry("c", 1, "c"), Entry("e", 1, "e")},
                               {Entry("b", 1, "b"), Entry("d", 1, "d")}},
                              script);
    // Moves in both directions and direction changes, with seeks.
    std::vector<std::string> positions;
    for (const auto& move : std::vector<Status (*)(InternalIterator&)>{
             [](InternalIterator& it) { return it.SeekToFirst(); },
             [](InternalIterator& it) { return it.Next(); },
             [](InternalIterator& it) { return it.Prev(); },
             [](InternalIterator& it) { return it.SeekToLast(); },
             [](InternalIterator& it) { return it.Prev(); },
             [](InternalIterator& it) { return it.Next(); },
             [](InternalIterator& it) { return it.Seek(Key("c", 1)); }}) {
      const Status moved = move(*merged);
      if (!moved.has_value()) {
        EXPECT_EQ(moved.error().message(), "injected failure");
        EXPECT_FALSE(merged->valid());
        // The next seek starts over.
        EXPECT_EQ(After(merged->SeekToFirst(), *merged), "a@1=a");
        return positions;
      }
      positions.push_back(Describe(*merged));
    }
    return positions;
  };

  const auto reference = std::make_shared<MoveScript>();
  EXPECT_EQ(run(reference), (std::vector<std::string>{"a@1=a", "b@1=b", "a@1=a", "e@1=e", "d@1=d",
                                                      "e@1=e", "c@1=c"}));
  for (int failing = 1; failing <= reference->moves; ++failing) {
    SCOPED_TRACE(failing);
    const auto script = std::make_shared<MoveScript>();
    script->fail_at = failing;
    EXPECT_LT(run(script).size(), 7U);
  }
}

// Writes tables through an in-memory file system and reads them through a
// table cache that caches nothing, so that every table a read reaches is
// opened.
class TablesTest : public testing::Test {
 protected:
  FileMetadata WriteTable(std::uint64_t number,
                          std::initializer_list<std::pair<std::string_view, SequenceNumber>> keys,
                          ValueKind kind = ValueKind::Value) {
    MemTable memtable(BytewiseComparator());
    for (const auto& [user_key, sequence] : keys) {
      EXPECT_TRUE(memtable.Add(sequence, kind, AsBytes(user_key), AsBytes(user_key)).has_value());
    }
    TableCache cache(file_system_, directory_, comparator_, {}, 0);
    TableBuilderOptions options;
    options.block_size = 1;
    auto built =
        BuildTable(file_system_, directory_, comparator_, options, cache, memtable, number);
    EXPECT_TRUE(built.has_value() && built->has_value());
    return std::move(built).value().value();
  }

  std::shared_ptr<const Version> MakeVersion(
      std::initializer_list<std::pair<std::uint32_t, FileMetadata>> files) {
    VersionEdit edit;
    for (const auto& [level, file] : files) {
      EXPECT_TRUE(edit.AddFile(level, file).has_value());
    }
    VersionBuilder builder(comparator_, Version());
    EXPECT_TRUE(builder.Apply(edit).has_value());
    return std::make_shared<const Version>(builder.Build().value());
  }

  std::unique_ptr<InternalIterator> Level(const std::shared_ptr<const Version>& version,
                                          std::uint32_t level) {
    return NewLevelIterator(version, version->files(level), cache_, comparator_, {});
  }

  // The table files that were opened since `start`, in order.
  std::vector<std::uint64_t> OpenedSince(std::size_t start) const {
    std::vector<std::uint64_t> numbers;
    const std::vector<std::string>& operations = file_system_.operations();
    for (std::size_t index = start; index < operations.size(); ++index) {
      constexpr std::string_view Opened = "open_random_access ";
      if (operations[index].starts_with(Opened)) {
        numbers.push_back(ParseFileName(operations[index].substr(Opened.size())).value().number);
      }
    }
    return numbers;
  }

  std::size_t Now() const { return file_system_.operations().size(); }

  MemoryFileSystem file_system_;
  const std::filesystem::path directory_ = std::filesystem::path("db");
  InternalKeyComparator comparator_{BytewiseComparator()};
  TableCache cache_{file_system_, directory_, comparator_, {}, 0};
};

using LevelIteratorTest = TablesTest;

TEST_F(LevelIteratorTest, ReadsFilesInBothDirections) {
  const auto version = MakeVersion({{1, WriteTable(10, {{"a", 1}, {"b", 1}})},
                                    {1, WriteTable(11, {{"c", 1}})},
                                    {1, WriteTable(12, {{"d", 2}, {"d", 1}, {"e", 1}})}});
  const auto level = Level(version, 1);
  const std::vector<std::string> expected{"a@1=a", "b@1=b", "c@1=c", "d@2=d", "d@1=d", "e@1=e"};

  std::size_t start = Now();
  EXPECT_EQ(ScanForward(*level), expected);
  EXPECT_EQ(OpenedSince(start), (std::vector<std::uint64_t>{10, 11, 12}));
  start = Now();
  EXPECT_EQ(ScanBackward(*level), expected);
  EXPECT_EQ(OpenedSince(start), (std::vector<std::uint64_t>{12, 11, 10}));

  const auto none = NewLevelIterator(version, version->files(2), cache_, comparator_, {});
  EXPECT_EQ(After(none->SeekToFirst(), *none), "<end>");
  EXPECT_EQ(After(none->SeekToLast(), *none), "<end>");
  EXPECT_EQ(After(none->Seek(Key("a", 1)), *none), "<end>");
}

TEST_F(LevelIteratorTest, SeeksInTheFileThatMayHoldTheTarget) {
  const auto version = MakeVersion(
      {{1, WriteTable(10, {{"b", 1}, {"d", 1}})}, {1, WriteTable(11, {{"f", 2}, {"h", 1}})}});
  const auto level = Level(version, 1);

  // A seek into the file that is already open does not look it up again.
  for (const auto& [target, expected, opened] :
       std::vector<std::tuple<std::vector<std::byte>, std::string, std::vector<std::uint64_t>>>{
           {Key("a", 1), "b@1=b", {10}},
           {Key("c", 1), "d@1=d", {}},
           {Key("d", 1), "d@1=d", {}},
           {Key("e", 1), "f@2=f", {11}},
           {Key("f", 1), "h@1=h", {}},
           {Key("h", 1), "h@1=h", {}},
           {Key("i", 1), "<end>", {}}}) {
    SCOPED_TRACE(expected);
    const std::size_t start = Now();
    EXPECT_EQ(After(level->Seek(target), *level), expected);
    EXPECT_EQ(OpenedSince(start), opened);
  }

  // Seeks within the open file reuse it, and moving between files opens them.
  std::size_t start = Now();
  EXPECT_EQ(After(level->Seek(Key("b", 1)), *level), "b@1=b");
  EXPECT_EQ(After(level->Seek(Key("c", 1)), *level), "d@1=d");
  EXPECT_EQ(After(level->SeekToFirst(), *level), "b@1=b");
  EXPECT_EQ(OpenedSince(start), (std::vector<std::uint64_t>{10}));
  start = Now();
  EXPECT_EQ(After(level->Next(), *level), "d@1=d");
  EXPECT_EQ(After(level->Next(), *level), "f@2=f");
  EXPECT_EQ(After(level->Prev(), *level), "d@1=d");
  EXPECT_EQ(After(level->Prev(), *level), "b@1=b");
  EXPECT_EQ(After(level->Prev(), *level), "<end>");
  EXPECT_EQ(OpenedSince(start), (std::vector<std::uint64_t>{11, 10}));
}

TEST_F(LevelIteratorTest, ContinuesAfterATablesLastEntry) {
  // The recorded largest keys follow the tables' last entries.
  FileMetadata first = WriteTable(10, {{"b", 1}});
  first.largest = InternalKey::Create(AsBytes("e"), 1, ValueKind::Value).value();
  FileMetadata second = WriteTable(11, {{"f", 1}});
  second.largest = InternalKey::Create(AsBytes("x"), 1, ValueKind::Value).value();
  const auto version = MakeVersion({{1, first}, {1, second}});
  const auto level = Level(version, 1);

  const std::size_t start = Now();
  EXPECT_EQ(After(level->Seek(Key("c", 1)), *level), "f@1=f");
  EXPECT_EQ(OpenedSince(start), (std::vector<std::uint64_t>{10, 11}));
  EXPECT_EQ(After(level->Seek(Key("g", 1)), *level), "<end>");
}

TEST_F(LevelIteratorTest, RejectsEmptyTables) {
  std::vector<std::byte> empty_table;
  {
    class VectorFile final : public WritableFile {
     public:
      explicit VectorFile(std::vector<std::byte>& data) : data_(data) {}
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
    TableBuilder builder(std::make_unique<VectorFile>(empty_table), comparator_, {});
    ASSERT_TRUE(builder.Finish().has_value());
  }
  file_system_.Write(TableFileName(directory_, 11), empty_table);
  const FileMetadata before = WriteTable(10, {{"a", 1}});
  FileMetadata empty = WriteTable(99, {{"c", 1}});
  empty.number = 11;
  empty.file_size = empty_table.size();
  const FileMetadata after = WriteTable(12, {{"e", 1}});
  const auto version = MakeVersion({{1, before}, {1, empty}, {1, after}});
  const auto level = Level(version, 1);

  const auto expect_corruption = [&](const Status& moved) {
    ASSERT_FALSE(moved.has_value());
    EXPECT_EQ(moved.error().code(), ErrorCode::Corruption) << moved.error().ToString();
    EXPECT_FALSE(level->valid());
  };
  expect_corruption(level->Seek(Key("c", 1)));
  ASSERT_EQ(After(level->SeekToFirst(), *level), "a@1=a");
  expect_corruption(level->Next());
  ASSERT_EQ(After(level->SeekToLast(), *level), "e@1=e");
  expect_corruption(level->Prev());

  const auto only =
      NewLevelIterator(version, version->files(1).subspan(1, 1), cache_, comparator_, {});
  const auto expect_only_corruption = [&](const Status& moved) {
    ASSERT_FALSE(moved.has_value());
    EXPECT_EQ(moved.error().code(), ErrorCode::Corruption);
  };
  expect_only_corruption(only->SeekToFirst());
  expect_only_corruption(only->SeekToLast());
}

TEST_F(LevelIteratorTest, ReportsTableErrors) {
  FileMetadata missing = WriteTable(10, {{"a", 1}});
  missing.number = 13;
  const auto with_missing = MakeVersion({{1, missing}, {1, WriteTable(11, {{"c", 1}, {"d", 1}})}});
  const auto level = Level(with_missing, 1);
  const Status not_found = level->SeekToFirst();
  ASSERT_FALSE(not_found.has_value());
  EXPECT_EQ(not_found.error().code(), ErrorCode::NotFound);
  EXPECT_FALSE(level->valid());
  EXPECT_EQ(After(level->SeekToLast(), *level), "d@1=d");
}

TEST_F(LevelIteratorTest, FailsWithAnyFileOperation) {
  FileMetadata middle = WriteTable(21, {{"o", 1}, {"p", 1}});
  // The recorded largest key follows the last entry, so a seek for q@9 passes
  // that entry and probes the table's first entry.
  middle.largest = InternalKey::Create(AsBytes("q"), 5, ValueKind::Value).value();
  const auto version = MakeVersion(
      {{1, WriteTable(20, {{"m", 1}, {"n", 1}})}, {1, middle}, {1, WriteTable(22, {{"s", 1}})}});
  const auto level = Level(version, 1);
  struct Case {
    std::string name;
    // Positions the iterator before the move; nothing if it starts fresh.
    std::optional<std::vector<std::byte>> start;
    Status (*move)(InternalIterator&);
    std::string expected;
  };
  const std::vector<Case> cases{
      {"first", std::nullopt, [](InternalIterator& it) { return it.SeekToFirst(); }, "m@1=m"},
      {"last", std::nullopt, [](InternalIterator& it) { return it.SeekToLast(); }, "s@1=s"},
      {"seek", std::nullopt, [](InternalIterator& it) { return it.Seek(Key("p", 1)); }, "p@1=p"},
      {"next in a file", Key("m", 1), [](InternalIterator& it) { return it.Next(); }, "n@1=n"},
      {"next to a file", Key("n", 1), [](InternalIterator& it) { return it.Next(); }, "o@1=o"},
      {"prev in a file", Key("p", 1), [](InternalIterator& it) { return it.Prev(); }, "o@1=o"},
      {"prev to a file", Key("o", 1), [](InternalIterator& it) { return it.Prev(); }, "n@1=n"},
      {"seek in the open file", Key("o", 1),
       [](InternalIterator& it) { return it.Seek(Key("p", 1)); }, "p@1=p"},
      {"seek past a table", Key("o", 1), [](InternalIterator& it) { return it.Seek(Key("q", 9)); },
       "s@1=s"},
  };
  for (const Case& test : cases) {
    SCOPED_TRACE(test.name);
    const auto prepare = [&] {
      if (test.start.has_value()) {
        ASSERT_TRUE(level->Seek(*test.start).has_value());
        ASSERT_TRUE(level->valid());
      } else {
        // Leaves the iterator without an open table.
        ASSERT_TRUE(level->Seek(Key("zz", 1)).has_value());
      }
    };
    prepare();
    const std::size_t start = Now();
    ASSERT_EQ(After(test.move(*level), *level), test.expected);
    const std::size_t count = Now() - start;
    ASSERT_GT(count, 0U);
    for (std::size_t failing = 0; failing < count; ++failing) {
      SCOPED_TRACE(failing);
      prepare();
      file_system_.FailOperation(Now() + failing, Error::Io("injected failure"));
      const Status failed = test.move(*level);
      ASSERT_FALSE(failed.has_value());
      EXPECT_EQ(failed.error().message(), "injected failure");
      EXPECT_FALSE(level->valid());
    }
  }
}

TEST_F(LevelIteratorTest, HoldsItsVersion) {
  auto version = MakeVersion({{1, WriteTable(10, {{"a", 1}})}});
  const std::weak_ptr<const Version> held = version;
  const auto level = NewLevelIterator(version, version->files(1), cache_, comparator_, {});
  version.reset();

  EXPECT_FALSE(held.expired());
  EXPECT_EQ(After(level->SeekToFirst(), *level), "a@1=a");
}

TEST(MemTableIteratorTest, ReadsAndHoldsTheMemtable) {
  auto memtable = std::make_shared<MemTable>(BytewiseComparator());
  ASSERT_TRUE(memtable->Add(2, ValueKind::Value, AsBytes("b"), AsBytes("b")).has_value());
  ASSERT_TRUE(memtable->Add(1, ValueKind::Value, AsBytes("a"), AsBytes("a")).has_value());
  ASSERT_TRUE(memtable->Add(3, ValueKind::Deletion, AsBytes("c"), {}).has_value());
  const std::weak_ptr<const MemTable> held = memtable;
  const auto iterator = NewMemTableIterator(std::move(memtable));

  EXPECT_FALSE(held.expired());
  EXPECT_EQ(ScanForward(*iterator), (std::vector<std::string>{"a@1=a", "b@2=b", "c@3="}));
  EXPECT_EQ(ScanBackward(*iterator), (std::vector<std::string>{"a@1=a", "b@2=b", "c@3="}));
  EXPECT_EQ(After(iterator->Seek(Key("b", 5)), *iterator), "b@2=b");
  EXPECT_EQ(After(iterator->Seek(Key("b", 1)), *iterator), "c@3=");
}

using InternalIteratorTest = TablesTest;

TEST_F(InternalIteratorTest, MergesEverySource) {
  auto memtable = std::make_shared<MemTable>(BytewiseComparator());
  ASSERT_TRUE(memtable->Add(30, ValueKind::Value, AsBytes("m"), AsBytes("m")).has_value());
  ASSERT_TRUE(memtable->Add(31, ValueKind::Value, AsBytes("c"), AsBytes("c")).has_value());
  auto immutable = std::make_shared<MemTable>(BytewiseComparator());
  ASSERT_TRUE(immutable->Add(20, ValueKind::Value, AsBytes("c"), AsBytes("c")).has_value());
  const auto version = MakeVersion({{0, WriteTable(12, {{"b", 11}, {"z", 12}})},
                                    {0, WriteTable(13, {{"c", 13}})},
                                    {1, WriteTable(10, {{"a", 5}, {"c", 6}})},
                                    {1, WriteTable(11, {{"d", 7}})},
                                    {3, WriteTable(14, {{"c", 1}, {"y", 1}})}});

  const auto merged = NewInternalIterator(memtable, immutable, version, cache_, comparator_, {});
  const std::vector<std::string> expected{"a@5=a", "b@11=b", "c@31=c", "c@20=c", "c@13=c", "c@6=c",
                                          "c@1=c", "d@7=d",  "m@30=m", "y@1=y",  "z@12=z"};
  EXPECT_EQ(ScanForward(*merged), expected);
  EXPECT_EQ(ScanBackward(*merged), expected);
  EXPECT_EQ(After(merged->Seek(Key("c", 15)), *merged), "c@13=c");

  const auto without_immutable =
      NewInternalIterator(memtable, nullptr, version, cache_, comparator_, {});
  std::vector<std::string> fewer = expected;
  std::erase(fewer, "c@20=c");
  EXPECT_EQ(ScanForward(*without_immutable), fewer);
}

TEST_F(InternalIteratorTest, OpensNoTableWhenCreated) {
  const auto version =
      MakeVersion({{0, WriteTable(12, {{"b", 11}})}, {1, WriteTable(10, {{"a", 5}})}});
  const std::size_t start = Now();

  const auto merged = NewInternalIterator(std::make_shared<MemTable>(BytewiseComparator()), nullptr,
                                          version, cache_, comparator_, {});

  EXPECT_EQ(Now(), start);
  EXPECT_FALSE(merged->valid());
}

}  // namespace
}  // namespace modern_leveldb
