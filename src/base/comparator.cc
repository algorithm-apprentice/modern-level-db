#include "modern_leveldb/base/comparator.h"

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <optional>
#include <string_view>
#include <vector>

#include "base/bytes_internal.h"

namespace modern_leveldb {
namespace {

int CompareCommonPrefix(ByteView left, ByteView right) noexcept {
    const std::size_t size = std::min(left.size(), right.size());
    return size == 0 ? 0 : std::memcmp(left.data(), right.data(), size);
}

int CompareKeyLengths(std::size_t left, std::size_t right) noexcept {
    if (left < right) {
        return -1;
    }
    return left > right ? 1 : 0;
}

std::optional<std::byte> SeparatorSuccessor(unsigned int start, unsigned int limit) noexcept {
    if (start < 0xffU && start + 1U < limit) {
        return static_cast<std::byte>(start + 1U);
    }
    return std::nullopt;
}

struct ByteSuccessor {
    std::size_t index;
    std::byte value;
};

std::optional<ByteSuccessor> FindByteSuccessor(ByteView key) noexcept {
    for (std::size_t index = 0; index < key.size(); ++index) {
        const auto value = std::to_integer<unsigned int>(key[index]);
        if (value != 0xffU) {
            const ByteSuccessor successor{index, static_cast<std::byte>(value + 1U)};
            return successor;
        }
    }
    return std::nullopt;
}

void CommitShortKey(std::vector<std::byte>& key, std::size_t index, std::byte successor) {
    key[index] = successor;
    key.resize(index + 1U);
}

class BytewiseComparatorImpl final : public Comparator {
public:
    [[nodiscard]] int Compare(const ByteView& left, const ByteView& right) const noexcept override {
        const int order = CompareCommonPrefix(left, right);
        if (order != 0) {
            return order;
        }
        return CompareKeyLengths(left.size(), right.size());
    }

    [[nodiscard]] std::string_view Name() const noexcept override {
        return "leveldb.BytewiseComparator";
    }

    void FindShortestSeparator(std::vector<std::byte>& start, ByteView limit) const override {
        const std::size_t minimum_size = std::min(start.size(), limit.size());
        const std::size_t difference = SharedPrefixLength(start, limit);

        if (difference >= minimum_size) {
            return;
        }

        const auto start_byte = std::to_integer<unsigned int>(start[difference]);
        const auto limit_byte = std::to_integer<unsigned int>(limit[difference]);
        const auto successor = SeparatorSuccessor(start_byte, limit_byte);
        if (successor.has_value()) {
            CommitShortKey(start, difference, *successor);
        }
    }

    void FindShortSuccessor(std::vector<std::byte>& key) const override {
        const auto successor = FindByteSuccessor(key);
        if (successor.has_value()) {
            CommitShortKey(key, successor->index, successor->value);
        }
    }
};

}  // namespace

const Comparator& BytewiseComparator() noexcept {
    static const BytewiseComparatorImpl comparator;
    return comparator;
}

}  // namespace modern_leveldb
