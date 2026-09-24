#include "engine/compaction.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <memory>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <tuple>
#include <utility>
#include <vector>

#include "engine/build_table.h"
#include "engine/compaction_picker.h"
#include "engine/internal_iterator.h"
#include "engine/table_cache.h"
#include "format/internal_key.h"
#include "memory/memtable.h"
#include "metadata/version.h"
#include "metadata/version_edit.h"
#include "modern_leveldb/base/bytes.h"
#include "modern_leveldb/base/comparator.h"
#include "modern_leveldb/base/result.h"
#include "support/memory_file_system.h"
#include "support/scripted_iterator.h"
#include "table/table.h"

namespace modern_leveldb {
namespace {

using test_support::MemoryFileSystem;
using test_support::MoveScript;
using test_support::ScriptedEntry;
using test_support::ScriptedIterator;

constexpr std::uint64_t MiB = std::uint64_t{1} << 20U;
constexpr ValueKind Deleted = ValueKind::Deletion;
constexpr ValueKind Put = ValueKind::Value;

struct Entry {
  std::string_view key;
  SequenceNumber sequence;
  ValueKind kind = Put;
  std::string value = "v";
};

using Outputs = std::vector<std::vector<std::string>>;

InternalKey Key(std::string_view user_key, SequenceNumber sequence, ValueKind kind = Put) {
  return InternalKey::Create(AsBytes(user_key), sequence, kind).value();
}

std::vector<std::byte> Encoded(const InternalKey& key) {
  return std::vector<std::byte>(key.encoded().begin(), key.encoded().end());
}

// Describes an entry as "key@sequence=value" or "key@sequence deleted".
std::string Describe(ByteView internal_key, ByteView value, bool with_value) {
  const Result<ParsedInternalKey> parsed = ParseInternalKey(internal_key);
  EXPECT_TRUE(parsed.has_value());
  std::string text =
      std::string(AsStringView(parsed->user_key)) + "@" + std::to_string(parsed->sequence);
  if (parsed->kind == Deleted) {
    return text + " deleted";
  }
  return with_value ? text + "=" + std::string(AsStringView(value)) : text;
}

// The metadata of a file that the compaction never reads.
FileMetadata Metadata(std::uint64_t number, std::string_view smallest, std::string_view largest,
                      std::uint64_t size = 100, SequenceNumber largest_sequence = 1) {
  return FileMetadata{.number = number,
                      .file_size = size,
                      .smallest = Key(smallest, 100),
                      .largest = Key(largest, largest_sequence)};
}

// A database directory in memory with a table cache, and hooks that number
// outputs from 100 and count entries.
class Harness {
 public:
  Harness() { options.table_options.block_size = 1; }

  FileMetadata WriteTable(std::uint64_t number, std::initializer_list<Entry> entries) {
    MemTable memtable(BytewiseComparator());
    for (const Entry& entry : entries) {
      EXPECT_TRUE(memtable.Add(entry.sequence, entry.kind, AsBytes(entry.key), AsBytes(entry.value))
                      .has_value());
    }
    auto built = BuildTable(file_system, directory, comparator, options.table_options, cache,
                            memtable, number);
    EXPECT_TRUE(built.has_value() && built->has_value());
    return std::move(built).value().value();
  }

  std::shared_ptr<const Version> MakeVersion(
      std::initializer_list<std::pair<std::uint32_t, FileMetadata>> files) const {
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

  // Compacts the version's files with the numbers at `level` and `level + 1`,
  // with every file of `level + 2` as a grandparent.
  static Compaction MakeCompaction(std::shared_ptr<const Version> version, std::uint32_t level,
                                   const std::vector<std::uint64_t>& inputs,
                                   const std::vector<std::uint64_t>& next_inputs) {
    const auto select = [&](std::uint32_t from, const std::vector<std::uint64_t>& numbers) {
      std::vector<Version::File> files;
      for (const Version::File& file : version->files(from)) {
        if (std::ranges::find(numbers, file->number) != numbers.end()) {
          files.push_back(file);
        }
      }
      EXPECT_EQ(files.size(), numbers.size());
      return files;
    };
    std::vector<Version::File> level_inputs = select(level, inputs);
    std::vector<Version::File> next_level_inputs = select(level + 1, next_inputs);
    std::vector<Version::File> grandparents;
    if (level + 2 < NumLevels) {
      const std::span<const Version::File> below = version->files(level + 2);
      grandparents.assign(below.begin(), below.end());
    }
    InternalKey pointer = level_inputs.back()->largest;
    return Compaction{.level = level,
                      .version = std::move(version),
                      .inputs = {std::move(level_inputs), std::move(next_level_inputs)},
                      .grandparents = std::move(grandparents),
                      .compact_pointer = std::move(pointer)};
  }

  Result<VersionEdit> Run(const Compaction& compaction, InternalIterator& input,
                          SequenceNumber smallest_snapshot = MaxSequenceNumber) {
    return RunCompaction(file_system, directory, comparator, options, cache, compaction, input,
                         smallest_snapshot, hooks);
  }

  Result<VersionEdit> RunOverTables(const Compaction& compaction,
                                    SequenceNumber smallest_snapshot = MaxSequenceNumber) {
    const std::unique_ptr<InternalIterator> input =
        NewCompactionIterator(compaction, cache, comparator);
    return Run(compaction, *input, smallest_snapshot);
  }

  // A scripted input, ordered by the comparator it is given.
  static std::unique_ptr<ScriptedIterator> Input(
      const Comparator& order, std::initializer_list<Entry> entries,
      std::shared_ptr<MoveScript> script = std::make_shared<MoveScript>()) {
    std::vector<ScriptedEntry> scripted;
    for (const Entry& entry : entries) {
      const ByteView value = AsBytes(entry.value);
      scripted.push_back(
          ScriptedEntry{.key = Encoded(Key(entry.key, entry.sequence, entry.kind)),
                        .value = std::vector<std::byte>(value.begin(), value.end())});
    }
    return std::make_unique<ScriptedIterator>(order, std::move(scripted), std::move(script));
  }

  std::unique_ptr<ScriptedIterator> Input(std::initializer_list<Entry> entries) const {
    return Input(comparator, entries);
  }

  // The entries of each output that the edit adds.
  Outputs Contents(const VersionEdit& edit, bool with_values = true) {
    Outputs outputs;
    for (const NewFile& added : edit.new_files()) {
      auto table = cache.Find(added.file.number, added.file.file_size);
      EXPECT_TRUE(table.has_value());
      std::vector<std::string> entries;
      Table::Iterator entry(**table);
      Status moved = entry.SeekToFirst();
      while (moved.has_value() && entry.valid()) {
        entries.push_back(Describe(entry.key(), entry.value(), with_values));
        moved = entry.Next();
      }
      EXPECT_TRUE(moved.has_value());
      outputs.push_back(std::move(entries));
    }
    return outputs;
  }

  std::vector<std::string> OperationsSince(std::size_t start) const {
    return std::vector<std::string>(
        file_system.operations().begin() + static_cast<std::ptrdiff_t>(start),
        file_system.operations().end());
  }

  MemoryFileSystem file_system;
  const std::filesystem::path directory = std::filesystem::path("db");
  InternalKeyComparator comparator{BytewiseComparator()};
  BlockCache blocks{std::size_t{1} << 20U};
  TableOptions table_options{.filter_policy = std::nullopt, .block_cache = &blocks};
  TableCache cache{file_system, directory, comparator, table_options, 100};
  CompactionOptions options;
  std::vector<std::uint64_t> numbers;
  int entries_seen = 0;
  std::optional<int> stop_at_entry;
  CompactionHooks hooks{.new_file_number =
                            [this] {
                              numbers.push_back(100 + std::uint64_t{numbers.size()});
                              return numbers.back();
                            },
                        .before_entry = [this]() -> Status {
                          if (++entries_seen == stop_at_entry) {
                            return std::unexpected(Error::Aborted("stopped"));
                          }
                          return {};
                        }};
};

TEST(CompactionTest, MergesBothLevelsIntoAVerifiedOutput) {
  Harness harness;
  const FileMetadata upper = harness.WriteTable(10, {{"a", 5, Put, "a5"}, {"c", 6, Put, "c6"}});
  const FileMetadata lower = harness.WriteTable(20, {{"a", 2, Put, "a2"}, {"b", 3, Put, "b3"}});
  const FileMetadata last = harness.WriteTable(21, {{"d", 1, Put, "d1"}});
  const Compaction compaction = Harness::MakeCompaction(
      harness.MakeVersion({{1, upper}, {2, lower}, {2, last}}), 1, {10}, {20, 21});
  const std::size_t start = harness.file_system.operations().size();

  const Result<VersionEdit> edit = harness.RunOverTables(compaction);
  const std::vector<std::string> operations = harness.OperationsSince(start);

  ASSERT_TRUE(edit.has_value()) << edit.error().ToString();
  // Compaction reads do not fill the block cache.
  EXPECT_EQ(harness.blocks.total_charge(), 0U);
  EXPECT_EQ(edit->deleted_files(), (std::set<DeletedFile>{{.level = 1, .number = 10},
                                                          {.level = 2, .number = 20},
                                                          {.level = 2, .number = 21}}));
  ASSERT_EQ(edit->compact_pointers().size(), 1U);
  EXPECT_EQ(edit->compact_pointers()[0].level, 1U);
  EXPECT_EQ(harness.comparator.Compare(edit->compact_pointers()[0].key, Key("c", 6)), 0);
  ASSERT_EQ(edit->new_files().size(), 1U);
  const NewFile& output = edit->new_files()[0];
  EXPECT_EQ(output.level, 2U);
  EXPECT_EQ(output.file.number, 100U);
  EXPECT_EQ(harness.comparator.Compare(output.file.smallest, Key("a", 5)), 0);
  EXPECT_EQ(harness.comparator.Compare(output.file.largest, Key("d", 1)), 0);
  const auto contents = harness.file_system.Contents(harness.directory / "000100.ldb");
  ASSERT_TRUE(contents.has_value());
  EXPECT_EQ(output.file.file_size, contents->size());
  // The older version of "a" is dropped.
  EXPECT_EQ(harness.Contents(*edit), (Outputs{{"a@5=a5", "b@3=b3", "c@6=c6", "d@1=d1"}}));
  EXPECT_EQ(harness.entries_seen, 5);
  EXPECT_EQ(harness.numbers, (std::vector<std::uint64_t>{100}));

  // The output is verified, and then the directory is synced once.
  ASSERT_FALSE(operations.empty());
  EXPECT_EQ(operations.back(), "sync_directory db");
  EXPECT_EQ(std::ranges::count(operations, "sync_directory db"), 1);
  EXPECT_NE(std::ranges::find(operations, "open_random_access 000100.ldb"), operations.end());
}

TEST(CompactionTest, MergesOverlappingLevelZeroInputs) {
  Harness harness;
  const FileMetadata first = harness.WriteTable(1, {{"a", 7, Put, "a7"}, {"c", 8, Put, "c8"}});
  const FileMetadata second = harness.WriteTable(2, {{"b", 9, Put, "b9"}, {"c", 3, Put, "c3"}});
  const FileMetadata below = harness.WriteTable(10, {{"d", 1, Put, "d1"}});
  const Compaction compaction = Harness::MakeCompaction(
      harness.MakeVersion({{0, first}, {0, second}, {1, below}}), 0, {1, 2}, {10});

  const Result<VersionEdit> edit = harness.RunOverTables(compaction);

  ASSERT_TRUE(edit.has_value()) << edit.error().ToString();
  ASSERT_EQ(edit->new_files().size(), 1U);
  EXPECT_EQ(edit->new_files()[0].level, 1U);
  EXPECT_EQ(harness.Contents(*edit), (Outputs{{"a@7=a7", "b@9=b9", "c@8=c8", "d@1=d1"}}));

  // Without next-level inputs, the level-0 inputs merge alone.
  Harness alone;
  const FileMetadata upper = alone.WriteTable(1, {{"a", 7, Put, "a7"}});
  const FileMetadata lower = alone.WriteTable(2, {{"a", 3, Put, "a3"}, {"b", 2, Put, "b2"}});
  const Result<VersionEdit> merged = alone.RunOverTables(
      Harness::MakeCompaction(alone.MakeVersion({{0, upper}, {0, lower}}), 0, {1, 2}, {}));
  ASSERT_TRUE(merged.has_value()) << merged.error().ToString();
  EXPECT_EQ(alone.Contents(*merged), (Outputs{{"a@7=a7", "b@2=b2"}}));
}

TEST(CompactionTest, DropsOnlyEntriesThatNoSnapshotReads) {
  Harness harness;
  // Levels 3 and 4 hold "k4", "k7" to "k8", and "k9", so deletions of those
  // keys still hide older entries there.
  const Compaction compaction =
      Harness::MakeCompaction(harness.MakeVersion({{1, Metadata(10, "k1", "k9")},
                                                   {2, Metadata(20, "k1", "k9")},
                                                   {3, Metadata(30, "k4", "k4")},
                                                   {3, Metadata(31, "k7", "k8")},
                                                   {4, Metadata(40, "k9", "k9")}}),
                              1, {10}, {20});
  // "k0" at 10 is visible at the snapshot, so it hides "k0" at 8.
  const auto input = harness.Input({{"k0", 12},
                                    {"k0", 10},
                                    {"k0", 8},
                                    {"k1", 20},
                                    {"k1", 15},
                                    {"k1", 9},
                                    {"k1", 8},
                                    {"k2", 12, Deleted},
                                    {"k2", 5},
                                    {"k3", 7, Deleted},
                                    {"k3", 4},
                                    {"k4", 6, Deleted},
                                    {"k4", 2},
                                    {"k5", 10, Deleted},
                                    {"k6", 9, Deleted},
                                    {"k7", 3, Deleted},
                                    {"k9", 2, Deleted}});

  const Result<VersionEdit> edit = harness.Run(compaction, *input, 10);

  ASSERT_TRUE(edit.has_value()) << edit.error().ToString();
  EXPECT_EQ(harness.Contents(*edit),
            (Outputs{{"k0@12=v", "k0@10=v", "k1@20=v", "k1@15=v", "k1@9=v", "k2@12 deleted",
                      "k2@5=v", "k4@6 deleted", "k7@3 deleted", "k9@2 deleted"}}));
}

TEST(CompactionTest, WritesNothingWhenEveryEntryIsDropped) {
  Harness harness;
  const Compaction compaction =
      Harness::MakeCompaction(harness.MakeVersion({{1, Metadata(10, "a", "z")}}), 1, {10}, {});
  const auto input = harness.Input({{"a", 7, Deleted}, {"a", 4}});

  const Result<VersionEdit> edit = harness.Run(compaction, *input);

  ASSERT_TRUE(edit.has_value()) << edit.error().ToString();
  EXPECT_TRUE(edit->new_files().empty());
  EXPECT_EQ(edit->deleted_files(), (std::set<DeletedFile>{{.level = 1, .number = 10}}));
  EXPECT_TRUE(harness.numbers.empty());
  EXPECT_TRUE(harness.file_system.operations().empty());
}

TEST(CompactionTest, EndsOutputsAtTheTargetSize) {
  Harness harness;
  // Each 1,000-byte value fills a block of about 1,026 bytes.
  harness.options.target_file_size = 2500;
  const Compaction compaction =
      Harness::MakeCompaction(harness.MakeVersion({{1, Metadata(10, "a", "z")}}), 1, {10}, {});
  const std::string value(1000, 'x');
  const auto input = harness.Input({{"a", 1, Put, value},
                                    {"b", 1, Put, value},
                                    {"c", 1, Put, value},
                                    {"d", 1, Put, value},
                                    {"e", 1, Put, value},
                                    {"f", 1, Put, value},
                                    {"g", 1, Put, value}});

  const Result<VersionEdit> edit = harness.Run(compaction, *input);

  ASSERT_TRUE(edit.has_value()) << edit.error().ToString();
  EXPECT_EQ(harness.Contents(*edit, false),
            (Outputs{{"a@1", "b@1", "c@1"}, {"d@1", "e@1", "f@1"}, {"g@1"}}));
  EXPECT_EQ(harness.numbers, (std::vector<std::uint64_t>{100, 101, 102}));
  std::vector<std::uint64_t> numbers;
  for (const NewFile& output : edit->new_files()) {
    EXPECT_EQ(output.level, 2U);
    numbers.push_back(output.file.number);
  }
  EXPECT_EQ(numbers, harness.numbers);
  EXPECT_EQ(harness.comparator.Compare(edit->new_files()[1].file.smallest, Key("d", 1)), 0);
  EXPECT_EQ(harness.comparator.Compare(edit->new_files()[1].file.largest, Key("f", 1)), 0);
}

// Runs a level-1 compaction of the entries whose grandparents in level 3 have
// the given user keys, largest sequences, and sizes in MiB, with a target file
// size of 1 MiB, and returns the user keys of each output.
Outputs SplitForGrandparents(
    std::initializer_list<std::tuple<std::string_view, SequenceNumber, std::uint64_t>> grandparents,
    std::initializer_list<Entry> entries) {
  Harness harness;
  harness.options.target_file_size = MiB;
  VersionEdit edit;
  EXPECT_TRUE(edit.AddFile(1, Metadata(10, "0", "z")).has_value());
  std::uint64_t number = 30;
  for (const auto& [key, sequence, size] : grandparents) {
    EXPECT_TRUE(edit.AddFile(3, Metadata(number++, key, key, size * MiB, sequence)).has_value());
  }
  VersionBuilder builder(harness.comparator, Version());
  EXPECT_TRUE(builder.Apply(edit).has_value());
  auto version = builder.Build();
  EXPECT_TRUE(version.has_value());
  const Compaction compaction =
      Harness::MakeCompaction(std::make_shared<const Version>(std::move(*version)), 1, {10}, {});
  const auto input = harness.Input(entries);
  const Result<VersionEdit> result = harness.Run(compaction, *input);
  EXPECT_TRUE(result.has_value());
  Outputs outputs;
  for (std::vector<std::string>& output : harness.Contents(*result, false)) {
    for (std::string& key : output) {
      key = key.substr(0, key.find('@'));
    }
    outputs.push_back(std::move(output));
  }
  return outputs;
}

TEST(CompactionTest, EndsOutputsEarlyForTheirGrandparents) {
  // Passing both grandparents adds 12 MiB, over ten times the target size.
  EXPECT_EQ(
      SplitForGrandparents({{"a", 1, 6}, {"b", 1, 6}}, {{"a", 9}, {"b", 9}, {"c", 9}, {"d", 9}}),
      (Outputs{{"a", "b"}, {"c", "d"}}));
  // A total at the limit does not end the output.
  EXPECT_EQ(
      SplitForGrandparents({{"a", 1, 5}, {"b", 1, 5}}, {{"a", 9}, {"b", 9}, {"c", 9}, {"d", 9}}),
      (Outputs{{"a", "b", "c", "d"}}));
  // Grandparents passed before the first entry do not count.
  EXPECT_EQ(SplitForGrandparents({{"0", 1, 5}, {"b", 1, 6}}, {{"a", 9}, {"c", 9}}),
            (Outputs{{"a", "c"}}));
  // An entry passes a grandparent only after its largest key.
  EXPECT_EQ(SplitForGrandparents({{"b", 5, 11}}, {{"a", 9}, {"b", 5}, {"c", 9}}),
            (Outputs{{"a", "b"}, {"c"}}));
}

TEST(CompactionTest, PassesGrandparentsForDroppedEntries) {
  // The shadowed "b" at sequence 4 passes the second grandparent, which ends
  // at "b" sequence 5, and so ends the open output.
  EXPECT_EQ(
      SplitForGrandparents({{"a", 1, 6}, {"b", 5, 6}}, {{"a", 9}, {"b", 9}, {"b", 4}, {"c", 9}}),
      (Outputs{{"a", "b"}, {"c"}}));
  // Dropped deletions pass both grandparents before any output opens, which
  // restarts the sum, so later entries share one output.
  EXPECT_EQ(SplitForGrandparents(
                {{"b", 1, 6}, {"d", 1, 6}},
                {{"a", 9, Deleted}, {"c", 9, Deleted}, {"e", 9, Deleted}, {"f", 9}, {"g", 9}}),
            (Outputs{{"f", "g"}}));
  // Bytes passed while no output is open still count.
  EXPECT_EQ(SplitForGrandparents({{"b", 1, 6}, {"e", 1, 6}},
                                 {{"a", 9, Deleted}, {"c", 9, Deleted}, {"d", 9}, {"f", 9}}),
            (Outputs{{"d"}, {"f"}}));
}

TEST(CompactionTest, CallsTheHooksAndStopsAtAnError) {
  Harness harness;
  const Compaction compaction =
      Harness::MakeCompaction(harness.MakeVersion({{1, Metadata(10, "a", "z")}}), 1, {10}, {});
  harness.stop_at_entry = 3;
  const auto input = harness.Input({{"a", 1}, {"b", 1}, {"c", 1}, {"d", 1}});

  const Result<VersionEdit> edit = harness.Run(compaction, *input);

  ASSERT_FALSE(edit.has_value());
  EXPECT_EQ(edit.error().code(), ErrorCode::Aborted);
  EXPECT_EQ(harness.entries_seen, 3);
  EXPECT_EQ(harness.numbers, (std::vector<std::uint64_t>{100}));
}

TEST(CompactionTest, RejectsAKeyThatIsNotAnInternalKey) {
  Harness harness;
  const Compaction compaction =
      Harness::MakeCompaction(harness.MakeVersion({{1, Metadata(10, "a", "z")}}), 1, {10}, {});
  // Ordered by bytes, the malformed key follows "a".
  std::vector<ScriptedEntry> entries{
      {.key = Encoded(Key("a", 1)), .value = {}},
      {.key = std::vector<std::byte>(3, std::byte{'b'}), .value = {}}};
  ScriptedIterator input(BytewiseComparator(), std::move(entries));

  const Result<VersionEdit> edit = harness.Run(compaction, input);

  ASSERT_FALSE(edit.has_value());
  EXPECT_EQ(edit.error().code(), ErrorCode::Corruption);
  // The output that the compaction began stays for obsolete-file cleanup.
  EXPECT_TRUE(harness.file_system.Contents(harness.directory / "000100.ldb").has_value());
}

TEST(CompactionTest, ReturnsTheErrorOfEveryFailedInputMove) {
  const auto run = [](std::optional<int> fail_at) {
    Harness harness;
    const Compaction compaction =
        Harness::MakeCompaction(harness.MakeVersion({{1, Metadata(10, "a", "z")}}), 1, {10}, {});
    auto script = std::make_shared<MoveScript>();
    script->fail_at = fail_at;
    const auto input = Harness::Input(harness.comparator, {{"a", 1}, {"b", 1}, {"c", 1}}, script);
    Result<VersionEdit> edit = harness.Run(compaction, *input);
    return std::pair(std::move(edit), script->moves);
  };
  const auto [reference, moves] = run(std::nullopt);
  ASSERT_TRUE(reference.has_value());
  ASSERT_EQ(moves, 4);

  for (int failing = 1; failing <= moves; ++failing) {
    SCOPED_TRACE(failing);
    const auto [edit, ignored] = run(failing);
    ASSERT_FALSE(edit.has_value());
    EXPECT_EQ(edit.error().message(), "injected failure");
  }
}

TEST(CompactionTest, ReturnsTheErrorOfEveryFailedFileOperation) {
  // Returns the compaction's result, failing its file operation `failing` if
  // given, and the number of file operations it performed. Split by size,
  // every entry ends its own output inside the loop; otherwise a grandparent
  // ends the first output, and the last one ends after the loop.
  const auto run = [](bool split_by_size, std::optional<std::size_t> failing) {
    Harness harness;
    harness.options.target_file_size = split_by_size ? 1 : MiB;
    const FileMetadata upper = harness.WriteTable(10, {{"a", 5}, {"c", 6}});
    const FileMetadata lower = harness.WriteTable(20, {{"b", 3}});
    const Compaction compaction = Harness::MakeCompaction(
        harness.MakeVersion({{1, upper}, {2, lower}, {3, Metadata(30, "a", "a", 11 * MiB)}}), 1,
        {10}, {20});
    const std::size_t start = harness.file_system.operations().size();
    if (failing.has_value()) {
      harness.file_system.FailOperation(start + *failing, Error::Io("injected failure"));
    }
    Result<VersionEdit> edit = harness.RunOverTables(compaction);
    return std::pair(std::move(edit), harness.file_system.operations().size() - start);
  };

  for (const bool split_by_size : {true, false}) {
    SCOPED_TRACE(split_by_size);
    const auto [reference, operations] = run(split_by_size, std::nullopt);
    ASSERT_TRUE(reference.has_value()) << reference.error().ToString();
    ASSERT_EQ(reference->new_files().size(), split_by_size ? 3U : 2U);

    for (std::size_t failing = 0; failing < operations; ++failing) {
      SCOPED_TRACE(failing);
      const auto [edit, ignored] = run(split_by_size, failing);
      ASSERT_FALSE(edit.has_value());
      EXPECT_EQ(edit.error().message(), "injected failure");
    }
  }
}

}  // namespace
}  // namespace modern_leveldb
