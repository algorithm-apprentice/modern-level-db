#include "memory/memtable.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <latch>
#include <limits>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

#include "format/internal_key.h"
#include "modern_leveldb/base/bytes.h"
#include "modern_leveldb/base/coding.h"
#include "modern_leveldb/base/comparator.h"
#include "modern_leveldb/base/result.h"

namespace modern_leveldb {
namespace {

class CaseInsensitiveComparator final : public Comparator {
 public:
  int Compare(ByteView left, ByteView right) const noexcept override {
    const std::size_t common = std::min(left.size(), right.size());
    for (std::size_t index = 0; index < common; ++index) {
      const unsigned int left_byte = Fold(left[index]);
      const unsigned int right_byte = Fold(right[index]);
      if (left_byte < right_byte) {
        return -1;
      }
      if (left_byte > right_byte) {
        return 1;
      }
    }
    if (left.size() < right.size()) {
      return -1;
    }
    if (left.size() > right.size()) {
      return 1;
    }
    return 0;
  }

  std::string_view Name() const noexcept override { return "test.CaseInsensitiveComparator"; }

  void FindShortestSeparator(std::vector<std::byte>&, ByteView) const override {}
  void FindShortSuccessor(std::vector<std::byte>&) const override {}

 private:
  static unsigned int Fold(std::byte value) noexcept {
    unsigned int byte = std::to_integer<unsigned int>(value);
    if (byte >= static_cast<unsigned int>('A') &&
        byte <= static_cast<unsigned int>('Z')) {
      byte += static_cast<unsigned int>('a' - 'A');
    }
    return byte;
  }
};

static_assert(!std::is_copy_constructible_v<MemTable>);
static_assert(!std::is_copy_assignable_v<MemTable>);
static_assert(!std::is_move_constructible_v<MemTable>);
static_assert(!std::is_move_assignable_v<MemTable>);
static_assert(!std::is_constructible_v<MemTable, CaseInsensitiveComparator&&>);
static_assert(!std::is_constructible_v<MemTable, const CaseInsensitiveComparator&&>);
static_assert(!std::is_copy_constructible_v<MemTable::Iterator>);
static_assert(!std::is_move_constructible_v<MemTable::Iterator>);

LookupKey MakeLookup(ByteView user_key, SequenceNumber sequence) {
  auto lookup = LookupKey::Create(user_key, sequence);
  EXPECT_TRUE(lookup.has_value());
  return std::move(lookup).value();
}

LookupKey MakeLookup(std::string_view user_key, SequenceNumber sequence) {
  return MakeLookup(AsBytes(user_key), sequence);
}

InternalKey MakeInternalKey(ByteView user_key, SequenceNumber sequence, ValueKind kind) {
  auto key = InternalKey::Create(user_key, sequence, kind);
  EXPECT_TRUE(key.has_value());
  return std::move(key).value();
}

InternalKey MakeInternalKey(std::string_view user_key, SequenceNumber sequence,
                            ValueKind kind) {
  return MakeInternalKey(AsBytes(user_key), sequence, kind);
}

std::array<std::byte, 8> OrderedKey(std::uint64_t value) {
  std::array<std::byte, 8> key;
  for (std::size_t index = 0; index < key.size(); ++index) {
    const std::size_t shift = (key.size() - index - 1U) * 8U;
    key[index] = static_cast<std::byte>((value >> shift) & 0xffU);
  }
  return key;
}

std::array<std::byte, 16> CheckedValue(std::uint64_t value) {
  std::array<std::byte, 16> result;
  EncodeFixed64(std::span<std::byte, 8>(result.data(), 8), value);
  EncodeFixed64(std::span<std::byte, 8>(result.data() + 8, 8),
                (value * 0x9e3779b97f4a7c15ULL) ^ 0xd6e8feb86659fd93ULL);
  return result;
}

bool DecodeOrderedKey(ByteView key, std::uint64_t& value) {
  if (key.size() != sizeof(value)) {
    return false;
  }
  value = 0;
  for (const std::byte byte : key) {
    value = (value << 8U) | std::to_integer<std::uint64_t>(byte);
  }
  return true;
}

TEST(MemTableTest, EmptyTableMissesAndHasInvalidIterator) {
  MemTable table{BytewiseComparator()};
  const LookupKey lookup = MakeLookup("missing", MaxSequenceNumber);

  const MemTableLookup result = table.Lookup(lookup);

  EXPECT_EQ(result.kind, MemTableLookupKind::Missing);
  EXPECT_TRUE(result.value.empty());
  EXPECT_GT(table.memory_usage(), 0U);

  MemTable::Iterator iterator(table);
  EXPECT_FALSE(iterator.valid());
  iterator.SeekToFirst();
  EXPECT_FALSE(iterator.valid());
  iterator.SeekToLast();
  EXPECT_FALSE(iterator.valid());

  const InternalKey target = MakeInternalKey("missing", MaxSequenceNumber, ValueKind::Value);
  ASSERT_TRUE(iterator.Seek(target.encoded()).has_value());
  EXPECT_FALSE(iterator.valid());
}

TEST(MemTableTest, ResolvesValuesDeletionsAndSnapshotBoundaries) {
  MemTable table{BytewiseComparator()};
  ASSERT_TRUE(table.Add(100, ValueKind::Value, AsBytes("key"), AsBytes("new")).has_value());
  ASSERT_TRUE(table.Add(90, ValueKind::Deletion, AsBytes("key"), {}).has_value());
  ASSERT_TRUE(table.Add(80, ValueKind::Value, AsBytes("key"), AsBytes("old")).has_value());
  ASSERT_TRUE(table.Add(70, ValueKind::Value, AsBytes("empty"), {}).has_value());

  const MemTableLookup newest = table.Lookup(MakeLookup("key", MaxSequenceNumber));
  EXPECT_EQ(newest.kind, MemTableLookupKind::Value);
  EXPECT_EQ(AsStringView(newest.value), "new");

  const MemTableLookup exact = table.Lookup(MakeLookup("key", 100));
  EXPECT_EQ(exact.kind, MemTableLookupKind::Value);
  EXPECT_EQ(AsStringView(exact.value), "new");

  const MemTableLookup deleted = table.Lookup(MakeLookup("key", 99));
  EXPECT_EQ(deleted.kind, MemTableLookupKind::Deletion);
  EXPECT_TRUE(deleted.value.empty());

  const MemTableLookup old = table.Lookup(MakeLookup("key", 89));
  EXPECT_EQ(old.kind, MemTableLookupKind::Value);
  EXPECT_EQ(AsStringView(old.value), "old");

  EXPECT_EQ(table.Lookup(MakeLookup("key", 79)).kind, MemTableLookupKind::Missing);
  EXPECT_EQ(table.Lookup(MakeLookup("absent", MaxSequenceNumber)).kind,
            MemTableLookupKind::Missing);

  const MemTableLookup empty = table.Lookup(MakeLookup("empty", MaxSequenceNumber));
  EXPECT_EQ(empty.kind, MemTableLookupKind::Value);
  EXPECT_TRUE(empty.value.empty());
}

TEST(MemTableTest, MovedFromLookupKeyRemainsAValidCanonicalLookup) {
  MemTable table{BytewiseComparator()};
  ASSERT_TRUE(table.Add(0, ValueKind::Value, {}, AsBytes("empty-key")).has_value());
  auto source = LookupKey::Create(AsBytes("other"), 99);
  ASSERT_TRUE(source.has_value());

  LookupKey destination = std::move(*source);
  const MemTableLookup moved_from = table.Lookup(*source);

  EXPECT_EQ(AsStringView(destination.user_key()), "other");
  EXPECT_EQ(moved_from.kind, MemTableLookupKind::Value);
  EXPECT_EQ(AsStringView(moved_from.value), "empty-key");
}

TEST(MemTableTest, SupportsBinaryKeysValuesAndComparatorEquality) {
  const std::array binary_key{std::byte{0x00}, std::byte{0xff}, std::byte{0x10}};
  const std::array binary_value{std::byte{0x80}, std::byte{0x00}, std::byte{0x7f}};
  MemTable binary_table{BytewiseComparator()};
  ASSERT_TRUE(
      binary_table.Add(4, ValueKind::Value, binary_key, binary_value).has_value());

  const MemTableLookup binary = binary_table.Lookup(MakeLookup(binary_key, 4));
  EXPECT_EQ(binary.kind, MemTableLookupKind::Value);
  EXPECT_TRUE(std::ranges::equal(binary.value, binary_value));

  CaseInsensitiveComparator comparator;
  MemTable folded_table(comparator);
  ASSERT_TRUE(
      folded_table.Add(7, ValueKind::Value, AsBytes("Key"), AsBytes("value")).has_value());

  const MemTableLookup folded = folded_table.Lookup(MakeLookup("kEy", 7));
  EXPECT_EQ(folded.kind, MemTableLookupKind::Value);
  EXPECT_EQ(AsStringView(folded.value), "value");
}

TEST(MemTableTest, IteratorUsesInternalKeyOrderAndPreservesStoredDeletionBytes) {
  MemTable table{BytewiseComparator()};
  ASSERT_TRUE(table.Add(2, ValueKind::Value, AsBytes("b"), AsBytes("b2")).has_value());
  ASSERT_TRUE(
      table.Add(5, ValueKind::Deletion, AsBytes("a"), AsBytes("delete-metadata")).has_value());
  ASSERT_TRUE(table.Add(4, ValueKind::Value, AsBytes("a"), AsBytes("a4")).has_value());
  ASSERT_TRUE(table.Add(3, ValueKind::Value, AsBytes("a"), AsBytes("a3")).has_value());
  ASSERT_TRUE(table.Add(1, ValueKind::Value, AsBytes("c"), AsBytes("c1")).has_value());

  struct Expected {
    std::string_view user_key;
    SequenceNumber sequence;
    ValueKind kind;
    std::string_view value;
  };
  constexpr std::array<Expected, 5> Forward{{
      {"a", 5, ValueKind::Deletion, "delete-metadata"},
      {"a", 4, ValueKind::Value, "a4"},
      {"a", 3, ValueKind::Value, "a3"},
      {"b", 2, ValueKind::Value, "b2"},
      {"c", 1, ValueKind::Value, "c1"},
  }};

  MemTable::Iterator iterator(table);
  iterator.SeekToFirst();
  for (const Expected& expected : Forward) {
    ASSERT_TRUE(iterator.valid());
    const auto parsed = ParseInternalKey(iterator.key());
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(AsStringView(parsed->user_key), expected.user_key);
    EXPECT_EQ(parsed->sequence, expected.sequence);
    EXPECT_EQ(parsed->kind, expected.kind);
    EXPECT_EQ(AsStringView(iterator.value()), expected.value);
    iterator.Next();
  }
  EXPECT_FALSE(iterator.valid());

  iterator.SeekToLast();
  for (auto expected = Forward.rbegin(); expected != Forward.rend(); ++expected) {
    ASSERT_TRUE(iterator.valid());
    const auto parsed = ParseInternalKey(iterator.key());
    ASSERT_TRUE(parsed.has_value());
    EXPECT_EQ(AsStringView(parsed->user_key), expected->user_key);
    EXPECT_EQ(parsed->sequence, expected->sequence);
    iterator.Prev();
  }
  EXPECT_FALSE(iterator.valid());

  const InternalKey exact = MakeInternalKey("a", 4, ValueKind::Value);
  ASSERT_TRUE(iterator.Seek(exact.encoded()).has_value());
  ASSERT_TRUE(iterator.valid());
  EXPECT_EQ(AsStringView(ParseInternalKey(iterator.key())->user_key), "a");
  EXPECT_EQ(ParseInternalKey(iterator.key())->sequence, 4U);

  const InternalKey before_newest = MakeInternalKey("a", 6, ValueKind::Value);
  ASSERT_TRUE(iterator.Seek(before_newest.encoded()).has_value());
  ASSERT_TRUE(iterator.valid());
  EXPECT_EQ(ParseInternalKey(iterator.key())->sequence, 5U);

  const InternalKey after_oldest = MakeInternalKey("a", 2, ValueKind::Value);
  ASSERT_TRUE(iterator.Seek(after_oldest.encoded()).has_value());
  ASSERT_TRUE(iterator.valid());
  EXPECT_EQ(AsStringView(ParseInternalKey(iterator.key())->user_key), "b");

  const MemTableLookup deletion = table.Lookup(MakeLookup("a", 5));
  EXPECT_EQ(deletion.kind, MemTableLookupKind::Deletion);
  EXPECT_TRUE(deletion.value.empty());
}

TEST(MemTableTest, RejectsInvalidAndDuplicateInternalKeys) {
  MemTable table{BytewiseComparator()};
  const std::size_t empty_usage = table.memory_usage();

  const Status bad_sequence =
      table.Add(MaxSequenceNumber + 1U, ValueKind::Value, AsBytes("key"), {});
  ASSERT_FALSE(bad_sequence.has_value());
  EXPECT_EQ(bad_sequence.error().code(), ErrorCode::InvalidArgument);
  EXPECT_EQ(table.memory_usage(), empty_usage);

  const Status bad_kind =
      table.Add(1, static_cast<ValueKind>(2), AsBytes("key"), AsBytes("value"));
  ASSERT_FALSE(bad_kind.has_value());
  EXPECT_EQ(bad_kind.error().code(), ErrorCode::InvalidArgument);
  EXPECT_EQ(table.memory_usage(), empty_usage);

  ASSERT_TRUE(table.Add(1, ValueKind::Value, AsBytes("key"), AsBytes("value")).has_value());
  const Status duplicate =
      table.Add(1, ValueKind::Value, AsBytes("key"), AsBytes("replacement"));
  ASSERT_FALSE(duplicate.has_value());
  EXPECT_EQ(duplicate.error().code(), ErrorCode::InvalidArgument);

  MemTable::Iterator iterator(table);
  iterator.SeekToFirst();
  ASSERT_TRUE(iterator.valid());
  EXPECT_EQ(AsStringView(iterator.value()), "value");
  iterator.Next();
  EXPECT_FALSE(iterator.valid());
}

TEST(MemTableTest, ReportsArenaReservedMemoryGrowth) {
  MemTable table{BytewiseComparator()};
  const std::size_t empty_usage = table.memory_usage();
  const std::vector<std::byte> large_value(5'000, std::byte{0x5a});

  ASSERT_TRUE(
      table.Add(1, ValueKind::Value, AsBytes("large"), large_value).has_value());

  EXPECT_GT(table.memory_usage(), empty_usage);
}

TEST(MemTableTest, MatchesRandomizedSnapshotModel) {
  struct Version {
    SequenceNumber sequence;
    ValueKind kind;
    std::string value;
  };

  constexpr std::size_t EntryCount = 1'000;
  constexpr std::size_t KeyCount = 41;
  MemTable table{BytewiseComparator()};
  std::unordered_map<std::string, std::vector<Version>> model;
  std::uint32_t random = 0x12345678U;

  for (SequenceNumber sequence = 1; sequence <= EntryCount; ++sequence) {
    random = random * 1664525U + 1013904223U;
    const std::string key = "key-" + std::to_string(random % KeyCount);
    const ValueKind kind =
        random % 5U == 0U ? ValueKind::Deletion : ValueKind::Value;
    const std::string value =
        kind == ValueKind::Value ? "value-" + std::to_string(sequence) : "";
    ASSERT_TRUE(table.Add(sequence, kind, AsBytes(key), AsBytes(value)).has_value());
    model[key].push_back({sequence, kind, value});
  }

  for (int query = 0; query < 2'000; ++query) {
    random = random * 1664525U + 1013904223U;
    const std::string key = "key-" + std::to_string(random % (KeyCount + 7U));
    const SequenceNumber snapshot = random % (EntryCount + 20U);
    const MemTableLookup actual = table.Lookup(MakeLookup(AsBytes(key), snapshot));

    const auto found = model.find(key);
    const Version* expected = nullptr;
    if (found != model.end()) {
      for (auto version = found->second.rbegin(); version != found->second.rend(); ++version) {
        if (version->sequence <= snapshot) {
          expected = &*version;
          break;
        }
      }
    }

    if (expected == nullptr) {
      EXPECT_EQ(actual.kind, MemTableLookupKind::Missing);
    } else if (expected->kind == ValueKind::Deletion) {
      EXPECT_EQ(actual.kind, MemTableLookupKind::Deletion);
      EXPECT_TRUE(actual.value.empty());
    } else {
      EXPECT_EQ(actual.kind, MemTableLookupKind::Value);
      EXPECT_EQ(AsStringView(actual.value), expected->value);
    }
  }
}

TEST(MemTableTest, ConcurrentReadersObservePublishedEntries) {
  constexpr std::uint64_t EntryCount = 1'024;
  constexpr int ReaderCount = 4;

  MemTable table{BytewiseComparator()};
  std::atomic<bool> done = false;
  std::atomic<int> failures = 0;
  std::atomic<int> read_iterations = 0;
  std::latch readers_ready(ReaderCount);
  std::latch readers_observed_first(ReaderCount);
  std::vector<std::jthread> readers;

  for (int reader_index = 0; reader_index < ReaderCount; ++reader_index) {
    readers.emplace_back([&, reader_index] {
      std::uint64_t random = static_cast<std::uint64_t>(reader_index + 1);
      readers_ready.count_down();

      const auto first_key = OrderedKey(0);
      const auto first_value = CheckedValue(0);
      while (true) {
        auto first_lookup = LookupKey::Create(first_key, MaxSequenceNumber);
        if (!first_lookup.has_value()) {
          failures.fetch_add(1, std::memory_order_relaxed);
          continue;
        }
        const MemTableLookup first = table.Lookup(*first_lookup);
        if (first.kind == MemTableLookupKind::Missing) {
          std::this_thread::yield();
          continue;
        }
        if (first.kind != MemTableLookupKind::Value ||
            !std::ranges::equal(first.value, first_value)) {
          failures.fetch_add(1, std::memory_order_relaxed);
        }
        readers_observed_first.count_down();
        break;
      }

      while (!done.load(std::memory_order_acquire)) {
        random = random * 6364136223846793005ULL + 1442695040888963407ULL;
        const std::uint64_t index = random % EntryCount;
        const auto key = OrderedKey(index);
        const auto expected_value = CheckedValue(index);
        auto lookup = LookupKey::Create(key, MaxSequenceNumber);
        if (!lookup.has_value()) {
          failures.fetch_add(1, std::memory_order_relaxed);
          continue;
        }

        const MemTableLookup result = table.Lookup(*lookup);
        if (result.kind == MemTableLookupKind::Value &&
            !std::ranges::equal(result.value, expected_value)) {
          failures.fetch_add(1, std::memory_order_relaxed);
        } else if (result.kind == MemTableLookupKind::Deletion) {
          failures.fetch_add(1, std::memory_order_relaxed);
        }

        MemTable::Iterator iterator(table);
        iterator.SeekToFirst();
        ByteView previous_user_key;
        while (iterator.valid()) {
          const auto parsed = ParseInternalKey(iterator.key());
          std::uint64_t observed_index = 0;
          if (!parsed.has_value() ||
              !DecodeOrderedKey(parsed->user_key, observed_index) ||
              parsed->sequence != observed_index + 1U ||
              parsed->kind != ValueKind::Value ||
              !std::ranges::equal(iterator.value(), CheckedValue(observed_index)) ||
              (!previous_user_key.empty() &&
               BytewiseComparator().Compare(previous_user_key, parsed->user_key) >= 0)) {
            failures.fetch_add(1, std::memory_order_relaxed);
            break;
          }
          previous_user_key = parsed->user_key;
          iterator.Next();
        }
        read_iterations.fetch_add(1, std::memory_order_relaxed);
      }
    });
  }

  readers_ready.wait();
  {
    const auto key = OrderedKey(0);
    const auto value = CheckedValue(0);
    ASSERT_TRUE(table.Add(1, ValueKind::Value, key, value).has_value());
  }
  readers_observed_first.wait();

  for (std::uint64_t index = 1; index < EntryCount; ++index) {
    const int previous_iterations = read_iterations.load(std::memory_order_relaxed);
    const auto key = OrderedKey(index);
    const auto value = CheckedValue(index);
    ASSERT_TRUE(table.Add(index + 1U, ValueKind::Value, key, value).has_value());
    if (index % 64U == 0U) {
      while (read_iterations.load(std::memory_order_relaxed) == previous_iterations) {
        std::this_thread::yield();
      }
      std::this_thread::yield();
    }
  }
  done.store(true, std::memory_order_release);
  readers.clear();

  EXPECT_EQ(failures.load(std::memory_order_relaxed), 0);
  EXPECT_GE(read_iterations.load(std::memory_order_relaxed), ReaderCount);

  MemTable::Iterator iterator(table);
  iterator.SeekToFirst();
  std::uint64_t count = 0;
  while (iterator.valid()) {
    ++count;
    iterator.Next();
  }
  EXPECT_EQ(count, EntryCount);
}

}  // namespace
}  // namespace modern_leveldb
