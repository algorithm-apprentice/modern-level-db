#include "engine/db_iterator.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <random>
#include <stdexcept>
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
static_assert(!std::is_constructible_v<DbIterator, std::unique_ptr<InternalIterator>,
                                       const Comparator&&, SequenceNumber, ReadSampling>);
static_assert(std::is_nothrow_constructible_v<DbIterator, std::unique_ptr<InternalIterator>,
                                              const Comparator&, SequenceNumber, ReadSampling>);

// Orders keys by their bytes in reverse.
class ReverseComparator final : public Comparator {
public:
    int Compare(const ByteView& left, const ByteView& right) const noexcept override {
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

class BufferedIterator final : public InternalIterator {
public:
    BufferedIterator(const Comparator& comparator, std::vector<ScriptedEntry> entries)
        : child_(comparator, std::move(entries)) {}

    bool valid() const noexcept override { return child_.valid(); }
    ByteView key() const noexcept override { return key_; }
    ByteView value() const noexcept override { return value_; }
    Status SeekToFirst() override { return Refresh(child_.SeekToFirst()); }
    Status SeekToLast() override { return Refresh(child_.SeekToLast()); }
    Status Seek(const ByteView& target) override { return Refresh(child_.Seek(target)); }
    Status Next() override { return Refresh(child_.Next()); }
    Status Prev() override { return Refresh(child_.Prev()); }

private:
    Status Refresh(Status moved) {
        if (child_.valid()) {
            const ByteView key = child_.key();
            const ByteView value = child_.value();
            key_.assign(key.begin(), key.end());
            value_.assign(value.begin(), value.end());
        } else {
            key_.clear();
            value_.clear();
        }
        return moved;
    }

    ScriptedIterator child_;
    std::vector<std::byte> key_;
    std::vector<std::byte> value_;
};

TEST_F(DbIteratorTest, ReverseOutputsOwnBinaryAndEmptyKeysAcrossReusedChildBuffers) {
    const std::string binary("\0x", 2);
    auto internal = std::make_unique<BufferedIterator>(
        comparator_,
        std::vector<ScriptedEntry>{Put("", 3, "empty3"), Put("", 1, "empty1"),
                                   Put(binary, 5, "binary"), Delete("b", 4), Put("b", 2, "hidden"),
                                   Put("c", 20, "future"), Put("c", 3, "visible")});
    DbIterator iterator(std::move(internal), BytewiseComparator(), 5);

    EXPECT_EQ(After(iterator.SeekToLast(), iterator), "c=visible");
    EXPECT_EQ(After(iterator.Prev(), iterator), binary + "=binary");
    EXPECT_EQ(After(iterator.Prev(), iterator), "=empty3");
    EXPECT_EQ(After(iterator.Next(), iterator), binary + "=binary");
    EXPECT_EQ(After(iterator.Prev(), iterator), "=empty3");
    EXPECT_EQ(After(iterator.Next(), iterator), binary + "=binary");
    EXPECT_EQ(After(iterator.Next(), iterator), "c=visible");
    EXPECT_EQ(After(iterator.Prev(), iterator), binary + "=binary");
}

TEST_F(DbIteratorTest, BeginningExhaustionStopsPositioningAndExplicitSeekRestoresIt) {
    auto script = std::make_shared<MoveScript>();
    auto iterator = Open({Put("", 3, "new"), Put("", 1, "old"), Put("b", 2, "next")}, 5, script);
    EXPECT_EQ(After(iterator->SeekToFirst(), *iterator), "=new");
    const int before = script->moves;

    EXPECT_EQ(After(iterator->Prev(), *iterator), "<end>");

    EXPECT_EQ(script->moves, before + 1);
    EXPECT_EQ(After(iterator->SeekToFirst(), *iterator), "=new");
    EXPECT_EQ(After(iterator->SeekToLast(), *iterator), "b=next");
    EXPECT_EQ(After(iterator->Prev(), *iterator), "=new");
    EXPECT_EQ(After(iterator->Next(), *iterator), "b=next");
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

TEST_F(DbIteratorTest, RepeatedSeeksPreserveBinaryLongAndBorrowedTargets) {
    const std::string binary("b\0x", 3);
    const std::string long_key(512, 'm');
    const auto iterator =
        Open({Put("", 3, "empty"), Put("b", 6, "short"), Put(binary, 5, "binary"),
              Put(long_key, 8, "future"), Put(long_key, 7, "long"), Put("z", 1, "last")},
             7);
    const std::vector<std::pair<std::string, std::string>> seeks{
        {"", "=empty"},   {binary, binary + "=binary"},   {long_key, long_key + "=long"},
        {"b", "b=short"}, {long_key, long_key + "=long"}, {"", "=empty"}};
    for (const auto& [target, expected] : seeks) {
        EXPECT_EQ(After(iterator->Seek(AsBytes(target)), *iterator), expected);
    }

    ASSERT_EQ(After(iterator->SeekToLast(), *iterator), "z=last");
    EXPECT_EQ(After(iterator->Seek(iterator->key()), *iterator), "z=last");
    ASSERT_EQ(After(iterator->SeekToLast(), *iterator), "z=last");
    ASSERT_EQ(After(iterator->Prev(), *iterator), long_key + "=long");
    EXPECT_EQ(After(iterator->Seek(iterator->key().subspan(1)), *iterator), long_key + "=long");
    ASSERT_EQ(After(iterator->Prev(), *iterator), binary + "=binary");
    EXPECT_EQ(After(iterator->Seek(iterator->key().first(2)), *iterator), binary + "=binary");
}

TEST_F(DbIteratorTest, ThrowingMovesInvalidateThePositionAndLaterSeeksRecover) {
    using Move = Status (*)(DbIterator&);
    const std::vector<std::pair<std::string_view, Move>> moves{
        {"first", [](DbIterator& it) { return it.SeekToFirst(); }},
        {"last", [](DbIterator& it) { return it.SeekToLast(); }},
        {"seek", [](DbIterator& it) { return it.Seek(AsBytes("c")); }},
        {"next", [](DbIterator& it) { return it.Next(); }},
        {"prev", [](DbIterator& it) { return it.Prev(); }}};
    for (const bool reverse : {false, true}) {
        for (const auto& [name, move] : moves) {
            SCOPED_TRACE(reverse);
            SCOPED_TRACE(name);
            auto script = std::make_shared<MoveScript>();
            const auto iterator = Open(Versions(), 5, script);
            ASSERT_TRUE((reverse ? iterator->SeekToLast() : iterator->SeekToFirst()).has_value());
            script->throw_at = script->moves + 1;

            EXPECT_THROW(static_cast<void>(move(*iterator)), std::runtime_error);

            EXPECT_FALSE(iterator->valid());
            script->throw_at.reset();
            EXPECT_EQ(After(iterator->SeekToFirst(), *iterator), "a=a5");
        }
    }
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

            DbIterator iterator(std::make_unique<ScriptedIterator>(comparator, entries),
                                *user_comparator, snapshot);
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
                    position =
                        found == model.end()
                            ? std::nullopt
                            : std::optional<std::size_t>(std::distance(model.begin(), found));
                } else if (operation < 5) {
                    moved = iterator.Next();
                    position =
                        *position + 1 == model.size() ? std::nullopt : std::optional(*position + 1);
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
    const auto short_iterator = Open({ScriptedEntry{.key = Bytes("x"), .value = Bytes("bad")}}, 10);
    const auto expect_corruption = [](DbIterator& iterator, const Status& moved) {
        ASSERT_FALSE(moved.has_value());
        EXPECT_EQ(moved.error().code(), ErrorCode::Corruption) << moved.error().ToString();
        EXPECT_FALSE(iterator.valid());
    };

    expect_corruption(*short_iterator, short_iterator->SeekToFirst());

    std::vector<std::byte> unknown = Put("c", 9, "bad").key;
    unknown[unknown.size() - InternalKeyTrailerSize] = std::byte{0x7f};
    const auto unknown_iterator =
        Open({ScriptedEntry{.key = std::move(unknown), .value = Bytes("bad")}}, 10);

    expect_corruption(*unknown_iterator, unknown_iterator->SeekToLast());
}

TEST_F(DbIteratorTest, RejectsMalformedKeyReachedWhileChangingToReverseDirectionAndRecovers) {
    auto malformed = Put("a", 1, "bad");
    malformed.key[malformed.key.size() - InternalKeyTrailerSize] = std::byte{0x7f};
    const auto expected = ParseInternalKey(malformed.key);
    ASSERT_FALSE(expected.has_value());
    const auto script = std::make_shared<MoveScript>();
    const auto iterator = Open({malformed, Put("b", 1, "visible")}, 10, script);

    for (int attempt = 0; attempt < 2; ++attempt) {
        ASSERT_EQ(After(iterator->Seek(AsBytes("b")), *iterator), "b=visible");
        const int before = script->moves;
        const Status result = iterator->Prev();
        ASSERT_FALSE(result.has_value());
        EXPECT_EQ(result.error().code(), expected.error().code());
        EXPECT_EQ(result.error().message(), expected.error().message());
        EXPECT_FALSE(iterator->valid());
        EXPECT_EQ(script->moves, before + 1);
    }
    EXPECT_EQ(After(iterator->Seek(AsBytes("b")), *iterator), "b=visible");
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
         {[](DbIterator& it) { return it.SeekToLast(); },
          [](DbIterator& it) { return it.Prev(); }}},
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

// Samples an iterator's reads with a fixed period and records the samples as
// "key@sequence".
class Sampler {
public:
    explicit Sampler(std::uint64_t period) : period_(period) {}

    ReadSampling Sampling() {
        return ReadSampling{.next_period =
                                [this] {
                                    ++periods_;
                                    return period_;
                                },
                            .sample =
                                [this](ByteView internal_key) {
                                    const InternalKeyView parsed =
                                        ParseInternalKey(internal_key).value();
                                    samples_.push_back(std::string(AsStringView(parsed.user_key)) +
                                                       "@" + std::to_string(parsed.sequence));
                                }};
    }

    [[nodiscard]] int periods() const { return periods_; }
    [[nodiscard]] const std::vector<std::string>& samples() const { return samples_; }

private:
    std::uint64_t period_;
    int periods_ = 0;
    std::vector<std::string> samples_;
};

std::unique_ptr<DbIterator> OpenSampled(std::vector<ScriptedEntry> entries, SequenceNumber sequence,
                                        Sampler& sampler) {
    static const InternalKeyComparator comparator(BytewiseComparator());
    return std::make_unique<DbIterator>(
        std::make_unique<ScriptedIterator>(comparator, std::move(entries)), BytewiseComparator(),
        sequence, sampler.Sampling());
}

using Samples = std::vector<std::string>;

// Every entry below has a 9-byte key and no value, so with a period of 9 bytes
// every entry that the iterator counts after the first is sampled once.
TEST(DbIteratorSamplingTest, SamplesTheEntriesThatItReadsForward) {
    Sampler sampler(9);
    const auto iterator = OpenSampled(
        {Delete("a", 3), Put("a", 2, ""), Put("b", 1, ""), Put("c", 50, ""), Put("c", 5, "")}, 10,
        sampler);
    EXPECT_EQ(sampler.periods(), 0);

    EXPECT_EQ(After(iterator->SeekToFirst(), *iterator), "b=");
    // The first entry draws the first period; hidden entries count too.
    EXPECT_EQ(sampler.samples(), (Samples{"a@2", "b@1"}));
    EXPECT_EQ(After(iterator->Next(), *iterator), "c=");
    EXPECT_EQ(sampler.samples(), (Samples{"a@2", "b@1", "c@50", "c@5"}));
    EXPECT_EQ(sampler.periods(), 5);
}

TEST(DbIteratorSamplingTest, SamplesTheEntriesThatItReadsBackward) {
    Sampler sampler(9);
    const auto iterator =
        OpenSampled({Put("a", 1, ""), Put("b", 1, ""), Put("c", 1, "")}, 10, sampler);

    EXPECT_EQ(After(iterator->SeekToLast(), *iterator), "c=");
    EXPECT_EQ(sampler.samples(), (Samples{"b@1"}));
    EXPECT_EQ(sampler.periods(), 2);
    EXPECT_EQ(After(iterator->Prev(), *iterator), "b=");
    EXPECT_EQ(After(iterator->Prev(), *iterator), "a=");
    EXPECT_EQ(sampler.samples(), (Samples{"b@1", "b@1", "a@1", "a@1"}));
    EXPECT_EQ(sampler.periods(), 5);
}

TEST(DbIteratorSamplingTest, DoesNotCountTheWalkThatChangesDirection) {
    Sampler sampler(9);
    const auto iterator = OpenSampled(
        {Put("a", 1, ""), Put("b", 3, ""), Put("b", 2, ""), Put("b", 1, ""), Put("c", 1, "")}, 1,
        sampler);
    EXPECT_EQ(After(iterator->SeekToFirst(), *iterator), "a=");
    EXPECT_EQ(After(iterator->Next(), *iterator), "b=");
    EXPECT_EQ(sampler.samples(), (Samples{"b@3", "b@2", "b@1"}));

    // Prev walks back over "b" to "a" before it counts "a".
    EXPECT_EQ(After(iterator->Prev(), *iterator), "a=");
    EXPECT_EQ(sampler.samples(), (Samples{"b@3", "b@2", "b@1", "a@1"}));
}

TEST(DbIteratorSamplingTest, SamplesALargeEntryOncePerPeriod) {
    Sampler sampler(3);
    // 9 key bytes and 11 value bytes pass six periods of 3 bytes after the first.
    const auto iterator = OpenSampled({Put("a", 1, "01234567890")}, 10, sampler);

    EXPECT_EQ(After(iterator->SeekToFirst(), *iterator), "a=01234567890");
    EXPECT_EQ(sampler.samples(), Samples(6, "a@1"));
    EXPECT_EQ(sampler.periods(), 7);
}

}  // namespace
}  // namespace modern_leveldb
