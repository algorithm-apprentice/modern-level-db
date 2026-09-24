#ifndef MODERN_LEVELDB_TESTS_SUPPORT_SCRIPTED_ITERATOR_H_
#define MODERN_LEVELDB_TESTS_SUPPORT_SCRIPTED_ITERATOR_H_

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <limits>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "engine/internal_iterator.h"
#include "modern_leveldb/base/bytes.h"
#include "modern_leveldb/base/comparator.h"
#include "modern_leveldb/base/result.h"

namespace modern_leveldb::test_support {

// Counts the moves of the iterators that share it and fails one of them.
struct MoveScript {
  int moves = 0;
  // The move, counting from one, that fails.
  std::optional<int> fail_at;
};

struct ScriptedEntry {
  std::vector<std::byte> key;
  std::vector<std::byte> value;
};

// An internal iterator over entries, which it sorts with the comparator. A
// failing move leaves it invalid.
class ScriptedIterator final : public InternalIterator {
 public:
  ScriptedIterator(const Comparator& comparator, std::vector<ScriptedEntry> entries,
                   std::shared_ptr<MoveScript> script = std::make_shared<MoveScript>())
      : comparator_(comparator), entries_(std::move(entries)), script_(std::move(script)) {
    std::ranges::stable_sort(entries_,
                             [this](const ScriptedEntry& left, const ScriptedEntry& right) {
                               return comparator_.Compare(left.key, right.key) < 0;
                             });
  }

  [[nodiscard]] bool valid() const noexcept override { return position_ < entries_.size(); }
  [[nodiscard]] ByteView key() const noexcept override { return entries_[position_].key; }
  [[nodiscard]] ByteView value() const noexcept override { return entries_[position_].value; }

  [[nodiscard]] Status SeekToFirst() override {
    return Move([this] { position_ = 0; });
  }
  [[nodiscard]] Status SeekToLast() override {
    return Move([this] { position_ = entries_.empty() ? End : entries_.size() - 1; });
  }
  [[nodiscard]] Status Seek(ByteView target) override {
    return Move([this, target] {
      const auto found = std::ranges::partition_point(entries_, [&](const ScriptedEntry& entry) {
        return comparator_.Compare(entry.key, target) < 0;
      });
      position_ = static_cast<std::size_t>(found - entries_.begin());
    });
  }
  [[nodiscard]] Status Next() override {
    return Move([this] { ++position_; });
  }
  [[nodiscard]] Status Prev() override {
    return Move([this] { position_ = position_ == 0 ? End : position_ - 1; });
  }

 private:
  static constexpr std::size_t End = std::numeric_limits<std::size_t>::max();

  template <typename Change>
  Status Move(Change change) {
    if (++script_->moves == script_->fail_at) {
      position_ = End;
      return std::unexpected(Error::Io("injected failure"));
    }
    change();
    return {};
  }

  const Comparator& comparator_;
  std::vector<ScriptedEntry> entries_;
  std::shared_ptr<MoveScript> script_;
  std::size_t position_ = End;
};

}  // namespace modern_leveldb::test_support

#endif  // MODERN_LEVELDB_TESTS_SUPPORT_SCRIPTED_ITERATOR_H_
