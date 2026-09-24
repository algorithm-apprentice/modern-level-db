#include "metadata/version_set.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <set>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "format/internal_key.h"
#include "metadata/filenames.h"
#include "metadata/version.h"
#include "metadata/version_edit.h"
#include "modern_leveldb/base/bytes.h"
#include "modern_leveldb/base/comparator.h"
#include "modern_leveldb/base/result.h"
#include "platform/file_system.h"
#include "support/memory_file_system.h"
#include "wal/wal_io.h"

namespace modern_leveldb {
namespace {

using test_support::MemoryFileSystem;

template <typename T>
concept CreatableWith = requires(FileSystem& file_system, T&& comparator) {
  VersionSet::Create(file_system, std::filesystem::path(), std::forward<T>(comparator));
};
template <typename T>
concept RecoverableWith = requires(FileSystem& file_system, T&& comparator) {
  VersionSet::Recover(file_system, std::filesystem::path(), std::forward<T>(comparator));
};

static_assert(!std::is_copy_constructible_v<VersionSet>);
static_assert(!std::is_move_constructible_v<VersionSet>);
static_assert(CreatableWith<const InternalKeyComparator&>);
static_assert(!CreatableWith<InternalKeyComparator>);
static_assert(RecoverableWith<const InternalKeyComparator&>);
static_assert(!RecoverableWith<InternalKeyComparator>);

std::vector<std::byte> Materialize(ByteView bytes) {
  return std::vector<std::byte>(bytes.begin(), bytes.end());
}

InternalKey Key(std::string_view user_key, SequenceNumber sequence) {
  auto key = InternalKey::Create(AsBytes(user_key), sequence, ValueKind::Value);
  EXPECT_TRUE(key.has_value());
  return std::move(key).value();
}

FileMetadata File(std::uint64_t number, std::string_view smallest, std::string_view largest) {
  return {.number = number,
          .file_size = number * 1000,
          .smallest = Key(smallest, 100),
          .largest = Key(largest, 100)};
}

std::string Text(const std::optional<std::vector<std::byte>>& bytes) {
  return bytes.has_value() ? std::string(AsStringView(*bytes)) : std::string("<missing>");
}

// The operations of one WAL record small enough for one fragment.
std::vector<std::string> RecordWrite(const std::string& file) {
  return {"append " + file, "append " + file, "flush " + file};
}

std::vector<std::string> Concat(std::initializer_list<std::vector<std::string>> parts) {
  std::vector<std::string> all;
  for (const std::vector<std::string>& part : parts) {
    all.insert(all.end(), part.begin(), part.end());
  }
  return all;
}

class VectorWritableFile final : public WritableFile {
 public:
  explicit VectorWritableFile(std::vector<std::byte>& data) : data_(data) {}
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

class VectorSequentialFile final : public SequentialFile {
 public:
  explicit VectorSequentialFile(std::vector<std::byte> data) : data_(std::move(data)) {}
  Result<std::size_t> Read(MutableByteView output) override {
    const std::size_t count = std::min(output.size(), data_.size() - offset_);
    std::copy_n(data_.begin() + static_cast<std::ptrdiff_t>(offset_), count, output.begin());
    offset_ += count;
    return count;
  }

 private:
  std::vector<std::byte> data_;
  std::size_t offset_ = 0;
};

std::vector<std::byte> Frame(const std::vector<std::vector<std::byte>>& records) {
  std::vector<std::byte> data;
  WalWriter writer(std::make_unique<VectorWritableFile>(data));
  for (const std::vector<std::byte>& record : records) {
    EXPECT_TRUE(writer.AddRecord(record).has_value());
  }
  return data;
}

struct FileSummary {
  std::uint64_t number;
  std::uint64_t file_size;
  std::vector<std::byte> smallest;
  std::vector<std::byte> largest;

  friend bool operator==(const FileSummary&, const FileSummary&) = default;
};

FileSummary Summarize(const FileMetadata& file) {
  return {.number = file.number,
          .file_size = file.file_size,
          .smallest = Materialize(file.smallest.encoded()),
          .largest = Materialize(file.largest.encoded())};
}

struct State {
  std::array<std::vector<FileSummary>, NumLevels> files;
  std::array<std::optional<std::vector<std::byte>>, NumLevels> compact_pointers;
  std::uint64_t log_number = 0;
  std::uint64_t prev_log_number = 0;
  SequenceNumber last_sequence = 0;

  friend bool operator==(const State&, const State&) = default;
};

State StateOf(const VersionSet& set) {
  State state;
  const std::shared_ptr<const Version> version = set.current();
  for (std::uint32_t level = 0; level < NumLevels; ++level) {
    for (const Version::File& file : version->files(level)) {
      state.files[level].push_back(Summarize(*file));
    }
    const std::optional<InternalKey>& pointer = set.compact_pointers()[level];
    if (pointer.has_value()) {
      state.compact_pointers[level] = Materialize(pointer->encoded());
    }
  }
  state.log_number = set.log_number();
  state.prev_log_number = set.prev_log_number();
  state.last_sequence = set.last_sequence();
  return state;
}

// Returns a file system that returns more bytes than asked from CURRENT.
class OversizedCurrentFileSystem final : public MemoryFileSystem {
 public:
  Result<std::unique_ptr<SequentialFile>> OpenSequential(
      const std::filesystem::path& path) override {
    if (path.filename() != "CURRENT") {
      return MemoryFileSystem::OpenSequential(path);
    }
    class Oversized final : public SequentialFile {
     public:
      Result<std::size_t> Read(MutableByteView output) override { return output.size() + 1; }
    };
    return std::make_unique<Oversized>();
  }
};

class VersionSetTest : public testing::Test {
 protected:
  std::unique_ptr<VersionSet> Create(FileSystem& file_system) {
    auto set = VersionSet::Create(file_system, directory_, comparator_);
    EXPECT_TRUE(set.has_value()) << set.error().ToString();
    return set.has_value() ? std::move(*set) : nullptr;
  }
  std::unique_ptr<VersionSet> Create() { return Create(file_system_); }

  Result<std::unique_ptr<VersionSet>> TryRecover(FileSystem& file_system) {
    return VersionSet::Recover(file_system, directory_, comparator_);
  }
  std::unique_ptr<VersionSet> Recover(FileSystem& file_system) {
    auto set = TryRecover(file_system);
    EXPECT_TRUE(set.has_value()) << set.error().ToString();
    return set.has_value() ? std::move(*set) : nullptr;
  }
  std::unique_ptr<VersionSet> Recover() { return Recover(file_system_); }

  void ExpectRecoveryError(ErrorCode code) {
    const auto set = TryRecover(file_system_);
    ASSERT_FALSE(set.has_value());
    EXPECT_EQ(set.error().code(), code) << set.error().ToString();
  }

  static void ExpectInvalid(VersionSet& set, const VersionEdit& edit) {
    const Status applied = set.LogAndApply(edit);
    ASSERT_FALSE(applied.has_value());
    EXPECT_EQ(applied.error().code(), ErrorCode::InvalidArgument) << applied.error().ToString();
  }

  void WriteManifest(std::uint64_t number, const std::vector<VersionEdit>& edits) {
    std::vector<std::vector<std::byte>> records;
    for (const VersionEdit& edit : edits) {
      records.push_back(edit.Encode());
    }
    file_system_.Write(DescriptorFileName(directory_, number), Frame(records));
  }

  void PointCurrentAt(std::uint64_t number) {
    file_system_.Write(CurrentFileName(directory_),
                       Materialize(AsBytes(CurrentFileContents(number))));
  }

  // Writes MANIFEST-000001 with the edits and points CURRENT at it.
  void WriteDatabase(const std::vector<VersionEdit>& edits) {
    WriteManifest(1, edits);
    PointCurrentAt(1);
  }

  std::vector<VersionEdit> ManifestRecords(std::uint64_t number) {
    const auto contents = file_system_.Contents(DescriptorFileName(directory_, number));
    EXPECT_TRUE(contents.has_value());
    WalReader reader(
        std::make_unique<VectorSequentialFile>(contents.value_or(std::vector<std::byte>())));
    std::vector<VersionEdit> edits;
    while (true) {
      auto event = reader.ReadNext();
      EXPECT_TRUE(event.has_value());
      if (!event.has_value() || !event->has_value()) {
        return edits;
      }
      const auto* record = std::get_if<WalLogicalRecord>(&**event);
      EXPECT_NE(record, nullptr);
      if (record == nullptr) {
        return edits;
      }
      auto edit = VersionEdit::Decode(record->data);
      EXPECT_TRUE(edit.has_value());
      edits.push_back(edit.value_or(VersionEdit()));
    }
  }

  // An edit with the counters that recovery requires.
  static VersionEdit Counters(std::uint64_t next_file, std::uint64_t log_number = 0,
                              SequenceNumber last_sequence = 0) {
    VersionEdit edit;
    edit.SetLogNumber(log_number);
    EXPECT_TRUE(edit.SetNextFileNumber(next_file).has_value());
    EXPECT_TRUE(edit.SetLastSequence(last_sequence).has_value());
    return edit;
  }

  // Creates a database with table 2 in level 1 and a compact pointer for level 1.
  void Populate(FileSystem& file_system) {
    auto set = Create(file_system);
    ASSERT_NE(set, nullptr);
    VersionEdit edit;
    ASSERT_TRUE(edit.AddFile(1, File(set->NewFileNumber(), "a", "c")).has_value());
    ASSERT_TRUE(edit.AddCompactPointer(1, Key("b", 5)).has_value());
    ASSERT_TRUE(set->LogAndApply(edit).has_value());
  }

  // An edit that the recovered populated database accepts.
  static VersionEdit Growth(VersionSet& set) {
    VersionEdit edit;
    EXPECT_TRUE(edit.AddFile(0, File(set.NewFileNumber(), "d", "f")).has_value());
    EXPECT_TRUE(edit.AddCompactPointer(2, Key("e", 6)).has_value());
    set.SetLastSequence(set.last_sequence() + 10);
    return edit;
  }

  MemoryFileSystem file_system_;
  const std::filesystem::path directory_ = std::filesystem::path("db");
  InternalKeyComparator comparator_{BytewiseComparator()};
};

TEST_F(VersionSetTest, CreatesANewDatabase) {
  const auto set = Create();
  ASSERT_NE(set, nullptr);

  EXPECT_EQ(file_system_.operations(),
            Concat({{"open_writable MANIFEST-000001"},
                    RecordWrite("MANIFEST-000001"),
                    RecordWrite("MANIFEST-000001"),
                    {"sync MANIFEST-000001", "sync_directory db", "open_writable 000001.dbtmp",
                     "append 000001.dbtmp", "sync 000001.dbtmp", "close 000001.dbtmp",
                     "rename 000001.dbtmp CURRENT", "sync_directory db"}}));
  EXPECT_EQ(Text(file_system_.Contents(directory_ / "CURRENT")), "MANIFEST-000001\n");
  EXPECT_FALSE(file_system_.Contents(directory_ / "000001.dbtmp").has_value());

  EXPECT_EQ(set->manifest_file_number(), 1U);
  EXPECT_EQ(StateOf(*set), State());
  EXPECT_TRUE(set->LiveFiles().empty());
  EXPECT_EQ(set->NewFileNumber(), 2U);

  const std::vector<VersionEdit> records = ManifestRecords(1);
  ASSERT_EQ(records.size(), 2U);
  EXPECT_EQ(records[0].comparator_name(), "leveldb.BytewiseComparator");
  EXPECT_EQ(records[1].log_number(), 0U);
  EXPECT_EQ(records[1].prev_log_number(), 0U);
  EXPECT_EQ(records[1].next_file_number(), 2U);
  EXPECT_EQ(records[1].last_sequence(), 0U);
}

TEST_F(VersionSetTest, ReportsFailuresWhileCreating) {
  file_system_.FailOperation(0, Error::Io("injected failure"));

  const auto set = VersionSet::Create(file_system_, directory_, comparator_);

  ASSERT_FALSE(set.has_value());
  EXPECT_EQ(set.error().message(), "injected failure");
  EXPECT_FALSE(file_system_.Contents(directory_ / "CURRENT").has_value());
}

TEST_F(VersionSetTest, RecordsEditsAndRecoversThem) {
  auto set = Create();
  ASSERT_NE(set, nullptr);
  const std::uint64_t log = set->NewFileNumber();
  const std::uint64_t table = set->NewFileNumber();
  VersionEdit edit;
  edit.SetLogNumber(log);
  edit.SetPrevLogNumber(1);
  ASSERT_TRUE(edit.AddFile(0, File(table, "a", "m")).has_value());
  ASSERT_TRUE(edit.AddCompactPointer(3, Key("k", 7)).has_value());
  set->SetLastSequence(42);
  const std::size_t start = file_system_.operations().size();

  ASSERT_TRUE(set->LogAndApply(edit).has_value());

  EXPECT_EQ(std::vector<std::string>(
                file_system_.operations().begin() + static_cast<std::ptrdiff_t>(start),
                file_system_.operations().end()),
            Concat({RecordWrite("MANIFEST-000001"), {"sync MANIFEST-000001"}}));
  State expected;
  expected.files[0].push_back(Summarize(File(table, "a", "m")));
  expected.compact_pointers[3] = Materialize(Key("k", 7).encoded());
  expected.log_number = log;
  expected.prev_log_number = 1;
  expected.last_sequence = 42;
  EXPECT_EQ(StateOf(*set), expected);
  EXPECT_EQ(set->LiveFiles(), (std::set<std::uint64_t>{table}));

  set.reset();
  const auto recovered = Recover();
  ASSERT_NE(recovered, nullptr);
  EXPECT_EQ(StateOf(*recovered), expected);
  EXPECT_EQ(recovered->manifest_file_number(), 4U);
  EXPECT_EQ(recovered->NewFileNumber(), 5U);
}

TEST_F(VersionSetTest, WritesANewManifestAfterRecovery) {
  Populate(file_system_);
  const auto first_manifest = file_system_.Contents(directory_ / "MANIFEST-000001");
  auto set = Recover();
  ASSERT_NE(set, nullptr);
  const State recovered = StateOf(*set);
  ASSERT_EQ(set->manifest_file_number(), 3U);
  const std::size_t start = file_system_.operations().size();

  ASSERT_TRUE(set->LogAndApply(VersionEdit()).has_value());

  EXPECT_EQ(std::vector<std::string>(
                file_system_.operations().begin() + static_cast<std::ptrdiff_t>(start),
                file_system_.operations().end()),
            Concat({{"open_writable MANIFEST-000003"},
                    RecordWrite("MANIFEST-000003"),
                    RecordWrite("MANIFEST-000003"),
                    {"sync MANIFEST-000003", "sync_directory db", "open_writable 000003.dbtmp",
                     "append 000003.dbtmp", "sync 000003.dbtmp", "close 000003.dbtmp",
                     "rename 000003.dbtmp CURRENT", "sync_directory db"}}));
  EXPECT_EQ(Text(file_system_.Contents(directory_ / "CURRENT")), "MANIFEST-000003\n");
  EXPECT_EQ(file_system_.Contents(directory_ / "MANIFEST-000001"), first_manifest);
  EXPECT_EQ(StateOf(*set), recovered);

  // The snapshot records the comparator, the compact pointers, and the files.
  const std::vector<VersionEdit> records = ManifestRecords(3);
  ASSERT_EQ(records.size(), 2U);
  EXPECT_EQ(records[0].comparator_name(), "leveldb.BytewiseComparator");
  ASSERT_EQ(records[0].compact_pointers().size(), 1U);
  EXPECT_EQ(records[0].compact_pointers()[0].level, 1U);
  ASSERT_EQ(records[0].new_files().size(), 1U);
  EXPECT_EQ(records[0].new_files()[0].level, 1U);
  EXPECT_EQ(Summarize(records[0].new_files()[0].file), Summarize(File(2, "a", "c")));
  EXPECT_FALSE(records[0].next_file_number().has_value());
  EXPECT_EQ(records[1].next_file_number(), 4U);

  const std::size_t later = file_system_.operations().size();
  ASSERT_TRUE(set->LogAndApply(VersionEdit()).has_value());
  EXPECT_EQ(file_system_.operations().size(), later + 4);
  set.reset();
  const auto reopened = Recover();
  ASSERT_NE(reopened, nullptr);
  EXPECT_EQ(StateOf(*reopened), recovered);
  EXPECT_EQ(reopened->manifest_file_number(), 4U);
}

TEST_F(VersionSetTest, KeepsOrFillsLogNumbers) {
  auto set = Create();
  ASSERT_NE(set, nullptr);
  const std::uint64_t log = set->NewFileNumber();
  VersionEdit first;
  first.SetLogNumber(log);
  first.SetPrevLogNumber(log - 1);
  ASSERT_TRUE(set->LogAndApply(first).has_value());

  ASSERT_TRUE(set->LogAndApply(VersionEdit()).has_value());

  EXPECT_EQ(set->log_number(), log);
  EXPECT_EQ(set->prev_log_number(), log - 1);
  const std::vector<VersionEdit> records = ManifestRecords(1);
  ASSERT_EQ(records.size(), 4U);
  EXPECT_EQ(records[3].log_number(), log);
  EXPECT_EQ(records[3].prev_log_number(), log - 1);
}

TEST_F(VersionSetTest, RejectsInvalidEditsWithoutWriting) {
  auto set = Create();
  ASSERT_NE(set, nullptr);
  const std::uint64_t log = set->NewFileNumber();
  const std::uint64_t table = set->NewFileNumber();
  VersionEdit setup = Counters(1, log);
  ASSERT_TRUE(setup.AddFile(1, File(table, "a", "c")).has_value());
  ASSERT_TRUE(set->LogAndApply(setup).has_value());
  const State before = StateOf(*set);
  const std::size_t operations = file_system_.operations().size();
  const std::uint64_t next = set->NewFileNumber() + 1;

  VersionEdit other_comparator;
  ASSERT_TRUE(other_comparator.SetComparatorName("other").has_value());
  ExpectInvalid(*set, other_comparator);
  VersionEdit older_log;
  older_log.SetLogNumber(log - 1);
  ExpectInvalid(*set, older_log);
  VersionEdit unallocated_log;
  unallocated_log.SetLogNumber(next);
  ExpectInvalid(*set, unallocated_log);
  VersionEdit unallocated_prev_log;
  unallocated_prev_log.SetPrevLogNumber(next);
  ExpectInvalid(*set, unallocated_prev_log);
  ExpectInvalid(*set, [&] {
    VersionEdit edit;
    EXPECT_TRUE(edit.AddFile(0, File(next, "x", "y")).has_value());
    return edit;
  }());
  ExpectInvalid(*set, [&] {
    VersionEdit edit;
    EXPECT_TRUE(edit.RemoveFile(0, table).has_value());
    return edit;
  }());
  ExpectInvalid(*set, [&] {
    VersionEdit edit;
    EXPECT_TRUE(edit.AddFile(1, File(next - 1, "b", "d")).has_value());
    return edit;
  }());

  EXPECT_EQ(file_system_.operations().size(), operations);
  EXPECT_EQ(StateOf(*set), before);
  VersionEdit same_comparator;
  ASSERT_TRUE(same_comparator.SetComparatorName("leveldb.BytewiseComparator").has_value());
  EXPECT_TRUE(set->LogAndApply(same_comparator).has_value());
}

TEST_F(VersionSetTest, MovesRecoveredFilesBetweenLevels) {
  Populate(file_system_);
  auto set = Recover();
  ASSERT_NE(set, nullptr);
  VersionEdit move;
  ASSERT_TRUE(move.RemoveFile(1, 2).has_value());
  ASSERT_TRUE(move.AddFile(2, File(2, "a", "c")).has_value());

  ASSERT_TRUE(set->LogAndApply(move).has_value());

  EXPECT_TRUE(set->current()->files(1).empty());
  ASSERT_EQ(set->current()->files(2).size(), 1U);
  EXPECT_EQ(set->current()->files(2)[0]->number, 2U);
}

TEST_F(VersionSetTest, StopsEditsAfterAFailedManifestCreation) {
  MemoryFileSystem reference;
  Populate(reference);
  auto reference_set = Recover(reference);
  ASSERT_NE(reference_set, nullptr);
  const State before = StateOf(*reference_set);
  const std::size_t start = reference.operations().size();
  ASSERT_TRUE(reference_set->LogAndApply(Growth(*reference_set)).has_value());
  const State after = StateOf(*reference_set);
  const std::vector<std::string> written(
      reference.operations().begin() + static_cast<std::ptrdiff_t>(start),
      reference.operations().end());
  const auto rename = std::ranges::find(written, "rename 000003.dbtmp CURRENT");
  ASSERT_NE(rename, written.end());
  const auto rename_index = static_cast<std::size_t>(rename - written.begin());

  for (std::size_t failing = 0; failing < written.size(); ++failing) {
    SCOPED_TRACE(written[failing]);
    MemoryFileSystem file_system;
    Populate(file_system);
    auto set = Recover(file_system);
    ASSERT_NE(set, nullptr);
    const VersionEdit edit = Growth(*set);
    file_system.FailOperation(file_system.operations().size() + failing,
                              Error::Io("injected failure"));

    const Status failed = set->LogAndApply(edit);

    ASSERT_FALSE(failed.has_value());
    EXPECT_EQ(failed.error().message(), "injected failure");
    State unchanged = before;
    unchanged.last_sequence = after.last_sequence;
    EXPECT_EQ(StateOf(*set), unchanged);
    const std::size_t operations = file_system.operations().size();
    const Status rejected = set->LogAndApply(VersionEdit());
    ASSERT_FALSE(rejected.has_value());
    EXPECT_EQ(rejected.error().message(), "injected failure");
    EXPECT_EQ(file_system.operations().size(), operations);
    for (const std::string& operation : file_system.operations()) {
      EXPECT_FALSE(operation.starts_with("remove")) << operation;
    }

    set.reset();
    const auto reopened = Recover(file_system);
    ASSERT_NE(reopened, nullptr);
    State expected = failing > rename_index ? after : before;
    EXPECT_EQ(StateOf(*reopened), expected);
  }
}

TEST_F(VersionSetTest, StopsEditsAfterAFailedAppend) {
  for (std::size_t failing = 0; failing < 4; ++failing) {
    SCOPED_TRACE(failing);
    MemoryFileSystem file_system;
    auto set = Create(file_system);
    ASSERT_NE(set, nullptr);
    const State before = StateOf(*set);
    VersionEdit edit;
    ASSERT_TRUE(edit.AddFile(0, File(set->NewFileNumber(), "a", "b")).has_value());
    file_system.FailOperation(file_system.operations().size() + failing,
                              Error::Io("injected failure"));

    ASSERT_FALSE(set->LogAndApply(edit).has_value());
    EXPECT_EQ(StateOf(*set), before);
    EXPECT_FALSE(set->LogAndApply(VersionEdit()).has_value());

    set.reset();
    const auto reopened = Recover(file_system);
    ASSERT_NE(reopened, nullptr);
    // A failed header or payload append leaves no complete record.
    EXPECT_EQ(reopened->current()->files(0).size(), failing < 2 ? 0U : 1U);
  }
}

TEST_F(VersionSetTest, ReportsMissingDatabases) { ExpectRecoveryError(ErrorCode::NotFound); }

TEST_F(VersionSetTest, RejectsInvalidCurrentFiles) {
  file_system_.Write(CurrentFileName(directory_), Materialize(AsBytes("MANIFEST-1\n")));
  ExpectRecoveryError(ErrorCode::Corruption);
  PointCurrentAt(9);
  ExpectRecoveryError(ErrorCode::Corruption);
}

TEST_F(VersionSetTest, ReturnsFileErrorsWhileRecovering) {
  Populate(file_system_);
  const std::size_t start = file_system_.operations().size();
  ASSERT_NE(Recover(), nullptr);
  const std::size_t count = file_system_.operations().size() - start;

  for (std::size_t failing = 0; failing < count; ++failing) {
    SCOPED_TRACE(failing);
    file_system_.FailOperation(file_system_.operations().size() + failing,
                               Error::Io("injected failure"));
    const auto set = TryRecover(file_system_);
    ASSERT_FALSE(set.has_value());
    EXPECT_EQ(set.error().message(), "injected failure");
  }

  OversizedCurrentFileSystem oversized;
  const auto set = TryRecover(oversized);
  ASSERT_FALSE(set.has_value());
  EXPECT_EQ(set.error().code(), ErrorCode::Io);
}

TEST_F(VersionSetTest, RejectsCorruptManifests) {
  Populate(file_system_);
  std::vector<std::byte> damaged = file_system_.Contents(directory_ / "MANIFEST-000001").value();
  damaged[10] ^= std::byte{0x01};
  file_system_.Write(directory_ / "MANIFEST-000001", damaged);
  ExpectRecoveryError(ErrorCode::Corruption);

  file_system_.Write(DescriptorFileName(directory_, 1), Frame({{std::byte{0xff}}}));
  ExpectRecoveryError(ErrorCode::Corruption);

  VersionEdit other_comparator = Counters(2);
  ASSERT_TRUE(other_comparator.SetComparatorName("other").has_value());
  WriteDatabase({other_comparator});
  ExpectRecoveryError(ErrorCode::InvalidArgument);

  VersionEdit missing_file = Counters(9);
  ASSERT_TRUE(missing_file.RemoveFile(1, 5).has_value());
  WriteDatabase({missing_file});
  ExpectRecoveryError(ErrorCode::Corruption);

  VersionEdit first = Counters(9);
  VersionEdit second;
  ASSERT_TRUE(first.AddFile(1, File(2, "a", "m")).has_value());
  ASSERT_TRUE(second.AddFile(1, File(3, "k", "z")).has_value());
  WriteDatabase({first, second});
  ExpectRecoveryError(ErrorCode::Corruption);
}

TEST_F(VersionSetTest, RequiresConsistentCounters) {
  VersionEdit no_next_file;
  no_next_file.SetLogNumber(0);
  ASSERT_TRUE(no_next_file.SetLastSequence(0).has_value());
  VersionEdit no_log_number;
  ASSERT_TRUE(no_log_number.SetNextFileNumber(2).has_value());
  ASSERT_TRUE(no_log_number.SetLastSequence(0).has_value());
  VersionEdit no_last_sequence;
  no_last_sequence.SetLogNumber(0);
  ASSERT_TRUE(no_last_sequence.SetNextFileNumber(2).has_value());
  VersionEdit prev_log_at_next = Counters(5, 1);
  prev_log_at_next.SetPrevLogNumber(5);
  VersionEdit file_at_next = Counters(5);
  ASSERT_TRUE(file_at_next.AddFile(0, File(5, "a", "b")).has_value());

  VersionEdit log_decreases;
  log_decreases.SetLogNumber(4);
  VersionEdit deleted_file_at_next = Counters(3);
  ASSERT_TRUE(deleted_file_at_next.AddFile(0, File(5, "a", "b")).has_value());
  VersionEdit deletion;
  ASSERT_TRUE(deletion.RemoveFile(0, 5).has_value());

  for (const std::vector<VersionEdit>& edits :
       std::vector<std::vector<VersionEdit>>{{no_next_file},
                                             {no_log_number},
                                             {no_last_sequence},
                                             {Counters(5, 5)},
                                             {prev_log_at_next},
                                             {file_at_next},
                                             {Counters(9, 5), log_decreases},
                                             {deleted_file_at_next, deletion},
                                             {Counters(FileNumberLimit + 1)}}) {
    SCOPED_TRACE(file_system_.operations().size());
    WriteDatabase(edits);
    ExpectRecoveryError(ErrorCode::Corruption);
  }

  WriteManifest(FileNumberLimit, {Counters(2)});
  PointCurrentAt(FileNumberLimit);
  ExpectRecoveryError(ErrorCode::Corruption);

  WriteDatabase({Counters(5, 4)});
  EXPECT_NE(Recover(), nullptr);
}

TEST_F(VersionSetTest, IgnoresALastRecordThatACrashCutShort) {
  auto set = Create();
  ASSERT_NE(set, nullptr);
  VersionEdit edit;
  ASSERT_TRUE(edit.AddFile(0, File(set->NewFileNumber(), "a", "b")).has_value());
  ASSERT_TRUE(set->LogAndApply(edit).has_value());
  set.reset();
  std::vector<std::byte> manifest = file_system_.Contents(directory_ / "MANIFEST-000001").value();
  manifest.resize(manifest.size() - 3);
  file_system_.Write(directory_ / "MANIFEST-000001", manifest);

  const auto recovered = Recover();

  ASSERT_NE(recovered, nullptr);
  EXPECT_TRUE(recovered->current()->files(0).empty());
}

TEST_F(VersionSetTest, NumbersTheNextManifestAboveTheRecoveredOne) {
  // Repairing an empty LevelDB database writes this MANIFEST.
  WriteDatabase({Counters(1)});
  const auto repaired = file_system_.Contents(directory_ / "MANIFEST-000001");
  auto set = Recover();
  ASSERT_NE(set, nullptr);

  EXPECT_EQ(set->manifest_file_number(), 2U);
  EXPECT_EQ(set->NewFileNumber(), 3U);
  ASSERT_TRUE(set->LogAndApply(VersionEdit()).has_value());
  EXPECT_EQ(Text(file_system_.Contents(directory_ / "CURRENT")), "MANIFEST-000002\n");
  EXPECT_EQ(file_system_.Contents(directory_ / "MANIFEST-000001"), repaired);
}

TEST_F(VersionSetTest, ReportsTheFilesOfHeldVersions) {
  auto set = Create();
  ASSERT_NE(set, nullptr);
  VersionEdit first;
  ASSERT_TRUE(first.AddFile(0, File(set->NewFileNumber(), "a", "b")).has_value());
  ASSERT_TRUE(first.AddFile(0, File(set->NewFileNumber(), "c", "d")).has_value());
  ASSERT_TRUE(set->LogAndApply(first).has_value());
  std::shared_ptr<const Version> held = set->current();
  VersionEdit second;
  ASSERT_TRUE(second.RemoveFile(0, 2).has_value());
  ASSERT_TRUE(second.AddFile(1, File(set->NewFileNumber(), "e", "f")).has_value());
  ASSERT_TRUE(set->LogAndApply(second).has_value());

  EXPECT_EQ(set->LiveFiles(), (std::set<std::uint64_t>{2, 3, 4}));
  held.reset();
  EXPECT_EQ(set->LiveFiles(), (std::set<std::uint64_t>{3, 4}));
  ASSERT_TRUE(set->LogAndApply(VersionEdit()).has_value());
  EXPECT_EQ(set->LiveFiles(), (std::set<std::uint64_t>{3, 4}));
}

TEST_F(VersionSetTest, AllocatesFileNumbersWithinTheLimit) {
  auto set = Create();
  ASSERT_NE(set, nullptr);

  ASSERT_TRUE(set->MarkFileNumberUsed(10).has_value());
  EXPECT_EQ(set->NewFileNumber(), 11U);
  ASSERT_TRUE(set->MarkFileNumberUsed(5).has_value());
  EXPECT_EQ(set->NewFileNumber(), 12U);
  const Status beyond = set->MarkFileNumberUsed(FileNumberLimit);
  ASSERT_FALSE(beyond.has_value());
  EXPECT_EQ(beyond.error().code(), ErrorCode::InvalidArgument);
  ASSERT_TRUE(set->MarkFileNumberUsed(FileNumberLimit - 1).has_value());

  // A MANIFEST may record the limit as its next file number, but no number
  // beyond it, so every MANIFEST that the version set writes is recoverable.
  ASSERT_TRUE(set->LogAndApply(VersionEdit()).has_value());
  EXPECT_EQ(set->NewFileNumber(), FileNumberLimit);
  ExpectInvalid(*set, VersionEdit());
  set.reset();
  const auto recovered = Recover();
  ASSERT_NE(recovered, nullptr);
  EXPECT_EQ(recovered->manifest_file_number(), FileNumberLimit);
  ExpectInvalid(*recovered, VersionEdit());
}

TEST_F(VersionSetTest, MatchesAnOrderedModel) {
  std::mt19937_64 random(0x5e7'2026);
  const auto below = [&](std::uint64_t bound) {
    return std::uniform_int_distribution<std::uint64_t>(0, bound - 1)(random);
  };
  auto set = Create();
  ASSERT_NE(set, nullptr);
  std::map<std::uint64_t, std::pair<std::uint32_t, FileMetadata>> live;
  State model;
  std::uint64_t slot = 0;

  for (int round = 0; round < 6; ++round) {
    for (int step = 0; step < 25; ++step) {
      VersionEdit edit;
      // Deletions and moves pick distinct files that were live before the edit.
      std::vector<std::uint64_t> candidates;
      for (const auto& entry : live) {
        candidates.push_back(entry.first);
      }
      for (std::uint64_t change = below(4); change > 0; --change) {
        const std::uint64_t kind = below(3);
        if (kind == 0 || candidates.empty()) {
          // Every file covers its own slot, so no two files overlap.
          const std::string prefix = "slot" + std::to_string(1000 + slot++);
          FileMetadata file = File(set->NewFileNumber(), prefix + "a", prefix + "z");
          const auto level = static_cast<std::uint32_t>(below(NumLevels));
          ASSERT_TRUE(edit.AddFile(level, file).has_value());
          live.emplace(file.number, std::pair(level, file));
        } else {
          const auto index = static_cast<std::ptrdiff_t>(below(candidates.size()));
          const std::uint64_t number = candidates[static_cast<std::size_t>(index)];
          candidates.erase(candidates.begin() + index);
          auto& entry = live.at(number);
          ASSERT_TRUE(edit.RemoveFile(entry.first, number).has_value());
          if (kind == 2) {
            const auto level = static_cast<std::uint32_t>(below(NumLevels));
            ASSERT_TRUE(edit.AddFile(level, entry.second).has_value());
            entry.first = level;
          } else {
            live.erase(number);
          }
        }
      }
      if (below(3) == 0) {
        const auto level = static_cast<std::uint32_t>(below(NumLevels));
        const InternalKey pointer = Key("pointer" + std::to_string(below(100)), below(50));
        ASSERT_TRUE(edit.AddCompactPointer(level, pointer).has_value());
        model.compact_pointers[level] = Materialize(pointer.encoded());
      }
      if (below(4) == 0) {
        model.log_number = set->NewFileNumber();
        edit.SetLogNumber(model.log_number);
      }
      model.last_sequence += below(100);
      set->SetLastSequence(model.last_sequence);

      ASSERT_TRUE(set->LogAndApply(edit).has_value());

      for (auto& files : model.files) {
        files.clear();
      }
      for (const auto& [number, entry] : live) {
        model.files[entry.first].push_back(Summarize(entry.second));
      }
      for (auto& files : model.files) {
        std::ranges::sort(files, [](const FileSummary& left, const FileSummary& right) {
          return left.smallest < right.smallest;
        });
      }
      ASSERT_EQ(StateOf(*set), model);
    }
    set.reset();
    set = Recover();
    ASSERT_NE(set, nullptr);
    ASSERT_EQ(StateOf(*set), model);
  }
}

}  // namespace
}  // namespace modern_leveldb
