#include "engine/lookup.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "engine/build_table.h"
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
#include "table/table.h"

namespace modern_leveldb {
namespace {

using test_support::MemoryFileSystem;

template <typename T>
concept LookupWith = requires(const MemTable& memtable, const Version& version, TableCache& cache,
                              T&& comparator, const LookupKey& key) {
  LookupValue(memtable, nullptr, version, cache, std::forward<T>(comparator), key);
};
static_assert(LookupWith<const InternalKeyComparator&>);
static_assert(!LookupWith<InternalKeyComparator>);

// Orders keys by their bytes in reverse.
class ReverseComparator final : public Comparator {
 public:
  int Compare(ByteView left, ByteView right) const noexcept override {
    return BytewiseComparator().Compare(right, left);
  }
  std::string_view Name() const noexcept override { return "test.Reverse"; }
  void FindShortestSeparator(std::vector<std::byte>&, ByteView) const override {}
  void FindShortSuccessor(std::vector<std::byte>&) const override {}
};

struct Entry {
  std::string_view key;
  SequenceNumber sequence;
  std::string_view value;
  ValueKind kind = ValueKind::Value;
};

Entry Deleted(std::string_view key, SequenceNumber sequence) {
  return {.key = key, .sequence = sequence, .value = {}, .kind = ValueKind::Deletion};
}

void Fill(MemTable& memtable, std::initializer_list<Entry> entries) {
  for (const Entry& entry : entries) {
    EXPECT_TRUE(memtable
                    .Add(entry.sequence, entry.kind, AsBytes(entry.key),
                         entry.kind == ValueKind::Value ? AsBytes(entry.value) : ByteView())
                    .has_value());
  }
}

// Looks up keys in tables that it writes through one file system, reporting
// which tables each lookup searches. Lookups use a table cache that caches
// nothing, so every table a lookup searches is opened.
class Database {
 public:
  explicit Database(const Comparator& user_comparator, TableOptions table_options = {})
      : user_comparator_(user_comparator),
        comparator_(user_comparator),
        table_options_(table_options),
        cache_(file_system_, directory_, comparator_, table_options_, 100),
        lookup_cache_(file_system_, directory_, comparator_, table_options_, 0) {}

  FileMetadata WriteTable(std::uint64_t number, std::initializer_list<Entry> entries) {
    MemTable memtable(user_comparator_);
    Fill(memtable, entries);
    auto built = BuildTable(file_system_, directory_, comparator_, {}, cache_, memtable, number);
    EXPECT_TRUE(built.has_value() && built->has_value());
    return std::move(built).value().value();
  }

  Version MakeVersion(std::initializer_list<std::pair<std::uint32_t, FileMetadata>> files) {
    VersionEdit edit;
    for (const auto& [level, file] : files) {
      EXPECT_TRUE(edit.AddFile(level, file).has_value());
    }
    VersionBuilder builder(comparator_, Version());
    EXPECT_TRUE(builder.Apply(edit).has_value());
    auto version = builder.Build();
    EXPECT_TRUE(version.has_value());
    return version.has_value() ? std::move(*version) : Version();
  }

  Result<std::optional<std::vector<std::byte>>> TryLookup(std::string_view user_key,
                                                          SequenceNumber sequence,
                                                          const Version& version,
                                                          const MemTable* memtable = nullptr,
                                                          const MemTable* immutable = nullptr,
                                                          const TableReadOptions& options = {}) {
    const MemTable empty(user_comparator_);
    auto key = LookupKey::Create(AsBytes(user_key), sequence);
    EXPECT_TRUE(key.has_value());
    read_start_ = file_system_.operations().size();
    return LookupValue(memtable != nullptr ? *memtable : empty, immutable, version, lookup_cache_,
                       comparator_, *key, options);
  }

  // Returns the value, or "<none>" for an absent key.
  std::string Lookup(std::string_view user_key, SequenceNumber sequence, const Version& version,
                     const MemTable* memtable = nullptr, const MemTable* immutable = nullptr,
                     const TableReadOptions& options = {}) {
    auto value = TryLookup(user_key, sequence, version, memtable, immutable, options);
    EXPECT_TRUE(value.has_value()) << value.error().ToString();
    if (!value.has_value() || !value->has_value()) {
      return "<none>";
    }
    return std::string(AsStringView(**value));
  }

  // The tables that the last lookup searched, by number.
  std::set<std::uint64_t> SearchedTables() const {
    std::set<std::uint64_t> numbers;
    const std::vector<std::string>& operations = file_system_.operations();
    for (std::size_t index = read_start_; index < operations.size(); ++index) {
      constexpr std::string_view Opened = "open_random_access ";
      if (operations[index].starts_with(Opened)) {
        const auto parsed = ParseFileName(operations[index].substr(Opened.size()));
        EXPECT_TRUE(parsed.has_value() && parsed->type == FileType::Table);
        numbers.insert(parsed.has_value() ? parsed->number : 0);
      }
    }
    return numbers;
  }

  const Comparator& user_comparator() const noexcept { return user_comparator_; }
  MemoryFileSystem& file_system() noexcept { return file_system_; }
  const std::filesystem::path& directory() const noexcept { return directory_; }

 private:
  const Comparator& user_comparator_;
  InternalKeyComparator comparator_;
  TableOptions table_options_;
  MemoryFileSystem file_system_;
  const std::filesystem::path directory_ = std::filesystem::path("db");
  TableCache cache_;
  TableCache lookup_cache_;
  std::size_t read_start_ = 0;
};

using Tables = std::set<std::uint64_t>;

TEST(LookupTest, ReadsTheMemtablesBeforeTheVersion) {
  Database database(BytewiseComparator());
  const Version version = database.MakeVersion({{0, database.WriteTable(5, {{"a", 1, "table"},
                                                                            {"b", 2, "table"},
                                                                            {"c", 3, "table"},
                                                                            {"d", 4, "table"},
                                                                            {"e", 5, "table"}})}});
  MemTable memtable(database.user_comparator());
  Fill(memtable, {{"a", 20, "memtable"}, Deleted("b", 21)});
  MemTable immutable(database.user_comparator());
  Fill(immutable, {{"a", 10, "immutable"}, {"c", 11, "immutable"}, Deleted("d", 12)});

  EXPECT_EQ(database.Lookup("a", 100, version, &memtable, &immutable), "memtable");
  EXPECT_EQ(database.Lookup("b", 100, version, &memtable, &immutable), "<none>");
  EXPECT_EQ(database.Lookup("c", 100, version, &memtable, &immutable), "immutable");
  EXPECT_EQ(database.Lookup("d", 100, version, &memtable, &immutable), "<none>");
  EXPECT_EQ(database.SearchedTables(), Tables{});
  EXPECT_EQ(database.Lookup("e", 100, version, &memtable, &immutable), "table");
  EXPECT_EQ(database.SearchedTables(), Tables{5});
  EXPECT_EQ(database.Lookup("f", 100, version, &memtable, &immutable), "<none>");
  EXPECT_EQ(database.SearchedTables(), Tables{});

  // Without an immutable memtable, the version decides.
  EXPECT_EQ(database.Lookup("c", 100, version, &memtable), "table");
  EXPECT_EQ(database.Lookup("d", 100, version, &memtable), "table");
}

TEST(LookupTest, SeesOnlyEntriesAtOrBeforeTheSequence) {
  Database database(BytewiseComparator());
  const Version version = database.MakeVersion(
      {{0, database.WriteTable(5, {{"deleted", 10, "table"}, {"key", 10, "table"}})}});
  MemTable memtable(database.user_comparator());
  Fill(memtable, {{"key", 30, "memtable"}, Deleted("deleted", 30)});
  MemTable immutable(database.user_comparator());
  Fill(immutable, {{"key", 20, "immutable"}});

  EXPECT_EQ(database.Lookup("key", 40, version, &memtable, &immutable), "memtable");
  EXPECT_EQ(database.Lookup("key", 30, version, &memtable, &immutable), "memtable");
  EXPECT_EQ(database.Lookup("key", 25, version, &memtable, &immutable), "immutable");
  EXPECT_EQ(database.Lookup("key", 15, version, &memtable, &immutable), "table");
  EXPECT_EQ(database.Lookup("key", 5, version, &memtable, &immutable), "<none>");
  EXPECT_EQ(database.Lookup("deleted", 40, version, &memtable, &immutable), "<none>");
  EXPECT_EQ(database.Lookup("deleted", 20, version, &memtable, &immutable), "table");
}

TEST(LookupTest, SearchesLevel0FilesFromNewestToOldest) {
  Database database(BytewiseComparator());
  const Version version = database.MakeVersion(
      {{0,
        database.WriteTable(5, {{"a", 1, "a5"}, {"k", 2, "k5"}, {"m", 4, "m5"}, {"z", 3, "z5"}})},
       {0, database.WriteTable(6, {{"x", 5, "x6"}})},
       {0, database.WriteTable(7, {{"k", 10, "k7"}, Deleted("m", 11)})}});

  EXPECT_EQ(database.Lookup("k", 100, version), "k7");
  EXPECT_EQ(database.SearchedTables(), Tables{7});
  EXPECT_EQ(database.Lookup("k", 5, version), "k5");
  EXPECT_EQ(database.SearchedTables(), (Tables{5, 7}));
  EXPECT_EQ(database.Lookup("m", 100, version), "<none>");
  EXPECT_EQ(database.SearchedTables(), Tables{7});
  EXPECT_EQ(database.Lookup("m", 10, version), "m5");
  // File 6 is newer than file 5, although file 5 sorts first by key.
  EXPECT_EQ(database.Lookup("x", 100, version), "x6");
  EXPECT_EQ(database.SearchedTables(), Tables{6});
  EXPECT_EQ(database.Lookup("b", 100, version), "<none>");
  EXPECT_EQ(database.SearchedTables(), Tables{5});
  EXPECT_EQ(database.Lookup("0", 100, version), "<none>");
  EXPECT_EQ(database.SearchedTables(), Tables{});
}

TEST(LookupTest, SearchesOneFilePerDeeperLevel) {
  Database database(BytewiseComparator());
  const Version version = database.MakeVersion(
      {{0, database.WriteTable(13, {{"c", 50, "c13"}})},
       {1, database.WriteTable(10, {{"b", 1, "b10"}, {"c", 1, "c10"}, {"d", 1, "d10"}})},
       {1, database.WriteTable(11, {{"f", 2, "f11"}, Deleted("g", 2), {"h", 2, "h11"}})},
       {2, database.WriteTable(
               12, {{"c", 3, "c12"}, {"e", 3, "e12"}, {"g", 1, "g12"}, {"i", 3, "i12"}})}});

  EXPECT_EQ(database.Lookup("c", 100, version), "c13");
  EXPECT_EQ(database.SearchedTables(), Tables{13});
  EXPECT_EQ(database.Lookup("c", 40, version), "c10");
  EXPECT_EQ(database.SearchedTables(), (Tables{10, 13}));
  // Between two files, before the first, and after the last.
  EXPECT_EQ(database.Lookup("e", 100, version), "e12");
  EXPECT_EQ(database.SearchedTables(), Tables{12});
  EXPECT_EQ(database.Lookup("a", 100, version), "<none>");
  EXPECT_EQ(database.SearchedTables(), Tables{});
  EXPECT_EQ(database.Lookup("i", 100, version), "i12");
  EXPECT_EQ(database.SearchedTables(), Tables{12});
  // A deletion hides a deeper value.
  EXPECT_EQ(database.Lookup("g", 100, version), "<none>");
  EXPECT_EQ(database.SearchedTables(), Tables{11});
  EXPECT_EQ(database.Lookup("g", 1, version), "g12");
}

TEST(LookupTest, FindsAUserKeyWhoseVersionsSpanTwoFiles) {
  Database database(BytewiseComparator());
  const Version version =
      database.MakeVersion({{1, database.WriteTable(20, {{"a", 1, "a20"}, {"u", 100, "u100"}})},
                            {1, database.WriteTable(21, {{"u", 50, "u50"}, {"z", 1, "z21"}})}});

  EXPECT_EQ(database.Lookup("u", 150, version), "u100");
  EXPECT_EQ(database.SearchedTables(), Tables{20});
  EXPECT_EQ(database.Lookup("u", 60, version), "u50");
  EXPECT_EQ(database.SearchedTables(), Tables{21});
  EXPECT_EQ(database.Lookup("u", 40, version), "<none>");
  EXPECT_EQ(database.SearchedTables(), Tables{21});
}

TEST(LookupTest, UsesTheComparatorsOrder) {
  // In this order, z < y < u < t < s < q < c.
  const ReverseComparator reverse;
  Database database(reverse);
  const Version version = database.MakeVersion(
      {{0, database.WriteTable(30, {{"z", 1, "z30"}, {"t", 2, "t30"}, {"s", 3, "s30"}})},
       {0, database.WriteTable(31, {{"t", 10, "t31"}})},
       {1, database.WriteTable(32, {{"y", 1, "y32"}, {"q", 100, "q100"}})},
       {1, database.WriteTable(33, {{"q", 50, "q50"}, {"c", 1, "c33"}})}});

  EXPECT_EQ(database.Lookup("t", 100, version), "t31");
  EXPECT_EQ(database.SearchedTables(), Tables{31});
  EXPECT_EQ(database.Lookup("z", 100, version), "z30");
  EXPECT_EQ(database.SearchedTables(), Tables{30});
  EXPECT_EQ(database.Lookup("u", 100, version), "<none>");
  EXPECT_EQ(database.SearchedTables(), (Tables{30, 32}));
  EXPECT_EQ(database.Lookup("b", 100, version), "<none>");
  EXPECT_EQ(database.SearchedTables(), Tables{});
  EXPECT_EQ(database.Lookup("q", 150, version), "q100");
  EXPECT_EQ(database.SearchedTables(), Tables{32});
  EXPECT_EQ(database.Lookup("q", 60, version), "q50");
  EXPECT_EQ(database.SearchedTables(), Tables{33});
  EXPECT_EQ(database.Lookup("c", 100, version), "c33");
  EXPECT_EQ(database.SearchedTables(), Tables{33});
}

TEST(LookupTest, ReturnsTableErrors) {
  Database database(BytewiseComparator());
  const FileMetadata level0 = database.WriteTable(40, {{"k", 5, "k40"}});
  const FileMetadata level1 = database.WriteTable(41, {{"a", 1, "a41"}, {"z", 1, "z41"}});
  FileMetadata missing = database.WriteTable(42, {{"a", 1, "a42"}, {"z", 1, "z42"}});
  missing.number = 43;
  database.file_system().Write(TableFileName(database.directory(), 44),
                               std::vector<std::byte>(100));
  FileMetadata damaged = missing;
  damaged.number = 44;
  damaged.file_size = 100;

  const Version with_missing = database.MakeVersion({{0, level0}, {1, level1}, {2, missing}});
  const auto not_found = database.TryLookup("m", 100, with_missing);
  ASSERT_FALSE(not_found.has_value());
  EXPECT_EQ(not_found.error().code(), ErrorCode::NotFound);

  const Version with_damaged = database.MakeVersion({{2, damaged}});
  const auto corrupt = database.TryLookup("m", 100, with_damaged);
  ASSERT_FALSE(corrupt.has_value());
  EXPECT_EQ(corrupt.error().code(), ErrorCode::Corruption);

  // A failure to open table 41, and a failure to read its data block, which is
  // the last operation of a lookup in it.
  const std::size_t start = database.file_system().operations().size();
  EXPECT_EQ(database.Lookup("a", 100, with_missing), "a41");
  const std::size_t count = database.file_system().operations().size() - start;
  for (const std::size_t failing : {std::size_t{0}, count - 1}) {
    SCOPED_TRACE(failing);
    database.file_system().FailOperation(database.file_system().operations().size() + failing,
                                         Error::Io("injected failure"));
    const auto failed = database.TryLookup("a", 100, with_missing);
    ASSERT_FALSE(failed.has_value());
    EXPECT_EQ(failed.error().message(), "injected failure");
  }

  // Tables after the deciding source are never opened.
  EXPECT_EQ(database.Lookup("k", 100, with_missing), "k40");
  EXPECT_EQ(database.Lookup("a", 100, with_missing), "a41");
  MemTable memtable(database.user_comparator());
  Fill(memtable, {{"m", 50, "memtable"}});
  EXPECT_EQ(database.Lookup("m", 100, with_missing, &memtable), "memtable");
}

TEST(LookupTest, PassesReadOptionsToTables) {
  BlockCache blocks(1 << 20);
  TableOptions table_options;
  table_options.block_cache = &blocks;
  Database database(BytewiseComparator(), table_options);
  const Version version = database.MakeVersion({{0, database.WriteTable(5, {{"k", 1, "v"}})}});

  EXPECT_EQ(database.Lookup("k", 10, version, nullptr, nullptr, {.fill_cache = false}), "v");
  EXPECT_EQ(blocks.total_charge(), 0U);
  EXPECT_EQ(database.Lookup("k", 10, version), "v");
  EXPECT_GT(blocks.total_charge(), 0U);
}

}  // namespace
}  // namespace modern_leveldb
