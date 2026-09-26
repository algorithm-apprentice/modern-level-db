#include "modern_leveldb/base/comparator.h"

#include <algorithm>
#include <compare>
#include <cstddef>
#include <string_view>
#include <vector>

namespace modern_leveldb {
namespace {

class BytewiseComparatorImpl final : public Comparator {
 public:
  [[nodiscard]] int Compare(ByteView left, ByteView right) const noexcept override {
    const auto ordering = std::lexicographical_compare_three_way(left.begin(), left.end(),
                                                                 right.begin(), right.end());
    if (ordering < 0) {
      return -1;
    }
    if (ordering > 0) {
      return 1;
    }
    return 0;
  }

  [[nodiscard]] std::string_view Name() const noexcept override {
    return "leveldb.BytewiseComparator";
  }

  void FindShortestSeparator(std::vector<std::byte>& start, ByteView limit) const override {
    const std::size_t minimum_size = std::min(start.size(), limit.size());
    std::size_t difference = 0;
    while (difference < minimum_size && start[difference] == limit[difference]) {
      ++difference;
    }

    if (difference >= minimum_size) {
      return;
    }

    const auto start_byte = std::to_integer<unsigned int>(start[difference]);
    const auto limit_byte = std::to_integer<unsigned int>(limit[difference]);
    if (start_byte < 0xffU && start_byte + 1U < limit_byte) {
      start[difference] = static_cast<std::byte>(start_byte + 1U);
      start.resize(difference + 1U);
    }
  }

  void FindShortSuccessor(std::vector<std::byte>& key) const override {
    for (std::size_t index = 0; index < key.size(); ++index) {
      const auto value = std::to_integer<unsigned int>(key[index]);
      if (value != 0xffU) {
        key[index] = static_cast<std::byte>(value + 1U);
        key.resize(index + 1U);
        return;
      }
    }
  }
};

}  // namespace

const Comparator& BytewiseComparator() noexcept {
  static const BytewiseComparatorImpl comparator;
  return comparator;
}

}  // namespace modern_leveldb
