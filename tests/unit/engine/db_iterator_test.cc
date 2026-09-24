#include "engine/db_iterator.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include "engine/internal_iterator.h"
#include "format/internal_key.h"
#include "modern_leveldb/base/bytes.h"
#include "modern_leveldb/base/comparator.h"
#include "modern_leveldb/base/result.h"
#include "support/scripted_iterator.h"

namespace modern_leveldb {
namespace {

using test_support::MoveScript;
using test_support::ScriptedEntry;
using test_support::ScriptedIterator;

static_assert(!std::is_copy_constructible_v<DbIterator>);
static_assert(!std::is_move_constructible_v<DbIterator>);
static_assert(std::is_constructible_v<DbIterator, std::unique_ptr<InternalIterator>,
                                      const Comparator&, SequenceNumber>);
static_assert(!std::is_constructible_v<DbIterator, std::unique_ptr<InternalIterator>,
                                       const Comparator&&, SequenceNumber>);

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

std::vector<std::byte> Bytes(std::string_view text) {
  return std::vector<std::byte>(AsBytes(text).begin(), AsBytes(text).end());
}

ScriptedEntry Put(std::string_view key, SequenceNumber sequence, std::string_view value) {
  const InternalKey internal =
      InternalKey::Create(AsBytes(key), sequence, ValueKind::Value).value();
  return {.key = Bytes(AsStringView(internal.encoded())), .value = Bytes(value)};
}

ScriptedEntry Delete(std::string_view key, SequenceNumber sequence) {
  const InternalKey internal =
      InternalKey::Create(AsBytes(key), sequence, ValueKind::Deletion).value();
  return {.key = Bytes(AsStringView(internal.encoded())), .value = {}};
}

// "key=value" for the iterator's position, or "<end>".
std::string Describe(const DbIterator& iterator) {
  if (!iterator.valid()) {
    return "<end>";
  }
  return std::string(AsStringView(iterator.key())) + "=" +
         std::string(AsStringView(iterator.value()));
}

std::string After(const Status& moved, const DbIterator& iterator) {
  EXPECT_TRUE(moved.has_value()) << moved.error().ToString();
  return Describe(iterator);
}

class DbIteratorTest : public testing::Test {
 protected:
  std::unique_ptr<DbIterator> Open(
      std::vector<ScriptedEntry> entries, SequenceNumber sequence,
      const std::shared_ptr<MoveScript>& script = std::make_shared<MoveScript>()) {
    return std::make_unique<DbIterator>(
        std::make_unique<ScriptedIterator>(comparator_, std::move(entries), script),
        comparator_.user_comparator(), sequence);
  }

  static std::vector<std::string> ScanForward(DbIterator& iterator) {
    std::vector<std::string> entries;
    Status status = iterator.SeekToFirst();
    while (status.has_value() && iterator.valid()) {
      entries.push_back(Describe(iterator));
      status = iterator.Next();
    }
    EXPECT_TRUE(status.has_value());
    return entries;
  }

  static std::vector<std::string> ScanBackward(DbIterator& iterator) {
    std::vector<std::string> entries;
    Status status = iterator.SeekToLast();
    while (status.has_value() && iterator.valid()) {
      entries.push_back(Describe(iterator));
      status = iterator.Prev();
    }
    EXPECT_TRUE(status.has_value());
    std::ranges::reverse(entries);
    return entries;
  }

  InternalKeyComparator comparator_{BytewiseComparator()};
};

std::vector<ScriptedEntry> Versions() {
  return {Put("a", 5, "a5"), Put("a", 3, "a3"), Delete("b", 4),   Put("b", 2, "b2"),
          Put("c", 6, "c6"), Put("c", 1, "c1"), Delete("d", 2),   Put("e", 7, "e7"),
          Delete("e", 5),    Put("e", 4, "e4"), Put("f", 5, "f5")};
}

TEST_F(DbIteratorTest, YieldsTheNewestVisibleValues) {
  const auto iterator = Open(Versions(), 5);
  const std::vector<std::string> expected{"a=a5", "c=c1", "f=f5"};
  EXPECT_EQ(ScanForward(*iterator), expected);
  EXPECT_EQ(ScanBackward(*iterator), expected);

  const auto later = Open(Versions(), 10);
  EXPECT_EQ(ScanForward(*later), (std::vector<std::string>{"a=a5", "c=c6", "e=e7", "f=f5"}));
  const auto earlier = Open(Versions(), 3);
  EXPECT_EQ(ScanForward(*earlier), (std::vector<std::string>{"a=a3", "b=b2", "c=c1"}));
  EXPECT_EQ(ScanBackward(*earlier), (std::vector<std::string>{"a=a3", "b=b2", "c=c1"}));
  const auto none = Open(Versions(), 0);
  EXPECT_TRUE(ScanForward(*none).empty());
  EXPECT_TRUE(ScanBackward(*none).empty());

  EXPECT_EQ(After(iterator->Seek(AsBytes("b")), *iterator), "c=c1");
  EXPECT_EQ(After(iterator->Seek(AsBytes("c")), *iterator), "c=c1");
  EXPECT_EQ(After(iterator->Seek(AsBytes("cc")), *iterator), "f=f5");
  EXPECT_EQ(After(iterator->Seek(AsBytes("g")), *iterator), "<end>");
}

TEST_F(DbIteratorTest, ChangesDirectionAtEveryKey) {
  const auto iterator = Open(Versions(), 5);
  EXPECT_EQ(After(iterator->SeekToFirst(), *iterator), "a=a5");
  EXPECT_EQ(After(iterator->Next(), *iterator), "c=c1");
  EXPECT_EQ(After(iterator->Prev(), *iterator), "a=a5");
  EXPECT_EQ(After(iterator->Next(), *iterator), "c=c1");
  EXPECT_EQ(After(iterator->Next(), *iterator), "f=f5");
  EXPECT_EQ(After(iterator->Prev(), *iterator), "c=c1");
  EXPECT_EQ(After(iterator->Prev(), *iterator), "a=a5");
  // Moving backward at the first key runs the internal iterator off the
  // beginning; moving forward again starts from its first entry.
  EXPECT_EQ(After(iterator->SeekToLast(), *iterator), "f=f5");
  EXPECT_EQ(After(iterator->Prev(), *iterator), "c=c1");
  EXPECT_EQ(After(iterator->Prev(), *iterator), "a=a5");
  EXPECT_EQ(After(iterator->Next(), *iterator), "c=c1");
  EXPECT_EQ(After(iterator->Prev(), *iterator), "a=a5");
  EXPECT_EQ(After(iterator->Prev(), *iterator), "<end>");
  EXPECT_EQ(After(iterator->SeekToFirst(), *iterator), "a=a5");
  EXPECT_EQ(After(iterator->Prev(), *iterator), "<end>");
  EXPECT_EQ(After(iterator->SeekToLast(), *iterator), "f=f5");
  EXPECT_EQ(After(iterator->Next(), *iterator), "<end>");
  EXPECT_EQ(After(iterator->Seek(AsBytes("c")), *iterator), "c=c1");
  EXPECT_EQ(After(iterator->Prev(), *iterator), "a=a5");
}

TEST_F(DbIteratorTest, MatchesAModel) {
  const ReverseComparator reverse;
  for (const Comparator* user_comparator :
       {&BytewiseComparator(), static_cast<const Comparator*>(&reverse)}) {
    const InternalKeyComparator comparator(*user_comparator);
    std::mt19937_64 random(0xdb17'2026);
    const auto below = [&](std::uint64_t bound) {
      return std::uniform_int_distribution<std::uint64_t>(0, bound - 1)(random);
    };
    for (int round = 0; round < 30; ++round) {
      SCOPED_TRACE(round);
      // The entries of every user key, with unique sequences.
      std::vector<ScriptedEntry> entries;
      std::map<std::string, std::map<SequenceNumber, std::optional<std::string>>> history;
      SequenceNumber sequence = 0;
      for (std::uint64_t count = below(60); count > 0; --count) {
        const std::string key = "k" + std::to_string(below(12));
        ++sequence;
        if (below(3) == 0) {
          entries.push_back(Delete(key, sequence));
          history[key][sequence] = std::nullopt;
        } else {
          const std::string value = key + "v" + std::to_string(sequence);
          entries.push_back(Put(key, sequence, value));
          history[key][sequence] = value;
        }
      }
      const SequenceNumber snapshot = below(sequence + 2);
      // The visible keys in the user comparator's order.
      std::vector<std::pair<std::string, std::string>> model;
      for (const auto& [key, versions] : history) {
        const auto newest = versions.upper_bound(snapshot);
        if (newest != versions.begin() && std::prev(newest)->second.has_value()) {
          model.emplace_back(key, *std::prev(newest)->second);
        }
      }
      std::ranges::sort(model, [&](const auto& left, const auto& right) {
        return user_comparator->Compare(AsBytes(left.first), AsBytes(right.first)) < 0;
      });
      const auto describe = [&](std::optional<std::size_t> position) {
        return position.has_value() ? model[*position].first + "=" + model[*position].second
                                    : std::string("<end>");
      };

      DbIterator iterator(std::make_unique<ScriptedIterator>(comparator, entries), *user_comparator,
                          snapshot);
      std::optional<std::size_t> position;
      for (int step = 0; step < 200; ++step) {
        const std::uint64_t operation = below(position.has_value() ? 6 : 3);
        Status moved;
        if (operation == 0) {
          moved = iterator.SeekToFirst();
          position = model.empty() ? std::nullopt : std::optional<std::size_t>(0);
        } else if (operation == 1) {
          moved = iterator.SeekToLast();
          position = model.empty() ? std::nullopt : std::optional(model.size() - 1);
        } else if (operation == 2) {
          const std::string target = "k" + std::to_string(below(13));
          moved = iterator.Seek(AsBytes(target));
          const auto found = std::ranges::find_if(model, [&](const auto& entry) {
            return user_comparator->Compare(AsBytes(entry.first), AsBytes(target)) >= 0;
          });
          position = found == model.end()
                         ? std::nullopt
                         : std::optional<std::size_t>(std::distance(model.begin(), found));
        } else if (operation < 5) {
          moved = iterator.Next();
          position = *position + 1 == model.size() ? std::nullopt : std::optional(*position + 1);
        } else {
          moved = iterator.Prev();
          position = *position == 0 ? std::nullopt : std::optional(*position - 1);
        }
        ASSERT_TRUE(moved.has_value());
        ASSERT_EQ(Describe(iterator), describe(position)) << "operation " << operation;
      }
    }
  }
}

TEST_F(DbIteratorTest, RejectsKeysThatAreNotInternalKeys) {
  // Keys that are not internal keys, a short one and one with an unknown kind,
  // sort before every internal key.
  std::vector<ScriptedEntry> entries = Versions();
  entries.push_back({.key = Bytes("x"), .value = Bytes("bad")});
  std::vector<std::byte> unknown = Put("c", 9, "bad").key;
  unknown[unknown.size() - InternalKeyTrailerSize] = std::byte{0x7f};
  entries.push_back({.key = unknown, .value = Bytes("bad")});
  const auto iterator = Open(entries, 10);
  const auto expect_corruption = [&](const Status& moved) {
    ASSERT_FALSE(moved.has_value());
    EXPECT_EQ(moved.error().code(), ErrorCode::Corruption) << moved.error().ToString();
    EXPECT_FALSE(iterator->valid());
  };

  // Moving forward from the first entry.
  expect_corruption(iterator->SeekToFirst());
  // Stepping back over the entries of the first user key.
  EXPECT_EQ(After(iterator->Seek(AsBytes("a")), *iterator), "a=a5");
  expect_corruption(iterator->Prev());
  // Looking past the first user key's entries for an earlier one.
  EXPECT_EQ(After(iterator->SeekToLast(), *iterator), "f=f5");
  EXPECT_EQ(After(iterator->Prev(), *iterator), "e=e7");
  EXPECT_EQ(After(iterator->Prev(), *iterator), "c=c6");
  expect_corruption(iterator->Prev());
}

TEST_F(DbIteratorTest, FailsWithAnyInternalMove) {
  using Move = Status (*)(DbIterator&);
  const std::vector<std::pair<std::string, std::vector<Move>>> scripts{
      {"forward",
       {[](DbIterator& it) { return it.SeekToFirst(); }, [](DbIterator& it) { return it.Next(); },
        [](DbIterator& it) { return it.Next(); }}},
      {"backward",
       {[](DbIterator& it) { return it.SeekToLast(); }, [](DbIterator& it) { return it.Prev(); },
        [](DbIterator& it) { return it.Prev(); }}},
      {"turns",
       {[](DbIterator& it) { return it.Seek(AsBytes("c")); },
        [](DbIterator& it) { return it.Prev(); }, [](DbIterator& it) { return it.Next(); },
        [](DbIterator& it) { return it.Next(); }, [](DbIterator& it) { return it.Prev(); }}},
      // A backward move that fails after copying a key and value.
      {"copied",
       {[](DbIterator& it) { return it.SeekToLast(); }, [](DbIterator& it) { return it.Prev(); }}},
  };
  for (const auto& [name, moves] : scripts) {
    SCOPED_TRACE(name);
    const auto run = [&](const std::shared_ptr<MoveScript>& script) {
      const auto iterator = Open(Versions(), 5, script);
      std::vector<std::string> positions;
      for (const Move move : moves) {
        const Status moved = move(*iterator);
        if (!moved.has_value()) {
          EXPECT_EQ(moved.error().message(), "injected failure");
          EXPECT_FALSE(iterator->valid());
          EXPECT_EQ(After(iterator->SeekToFirst(), *iterator), "a=a5");
          return positions;
        }
        positions.push_back(Describe(*iterator));
      }
      return positions;
    };
    const auto reference = std::make_shared<MoveScript>();
    const std::vector<std::string> expected = run(reference);
    ASSERT_EQ(expected.size(), moves.size());
    for (int failing = 1; failing <= reference->moves; ++failing) {
      SCOPED_TRACE(failing);
      const auto script = std::make_shared<MoveScript>();
      script->fail_at = failing;
      EXPECT_LT(run(script).size(), moves.size());
    }
  }
}

}  // namespace
}  // namespace modern_leveldb
