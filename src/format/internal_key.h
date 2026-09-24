#ifndef MODERN_LEVELDB_FORMAT_INTERNAL_KEY_H_
#define MODERN_LEVELDB_FORMAT_INTERNAL_KEY_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>
#include <utility>
#include <vector>

#include "modern_leveldb/base/bytes.h"
#include "modern_leveldb/base/comparator.h"
#include "modern_leveldb/base/result.h"

namespace modern_leveldb {

using SequenceNumber = std::uint64_t;

inline constexpr SequenceNumber MaxSequenceNumber = (std::uint64_t{1} << 56U) - 1U;
inline constexpr std::size_t InternalKeyTrailerSize = 8;

enum class ValueKind : std::uint8_t {
  Deletion = 0,
  Value = 1,
};

inline constexpr ValueKind SeekValueKind = ValueKind::Value;

struct ParsedInternalKey {
  ByteView user_key;
  SequenceNumber sequence;
  ValueKind kind;
};

[[nodiscard]] Result<ParsedInternalKey> ParseInternalKey(ByteView encoded);

class LookupKey final {
 public:
  [[nodiscard]] static Result<LookupKey> Create(ByteView user_key,
                                                SequenceNumber sequence);

  LookupKey(const LookupKey&) = delete;
  LookupKey& operator=(const LookupKey&) = delete;
  LookupKey(LookupKey&& source) noexcept;
  LookupKey& operator=(LookupKey&& source) noexcept;
  ~LookupKey() = default;

  [[nodiscard]] ByteView memtable_key() const noexcept;
  [[nodiscard]] ByteView internal_key() const noexcept;
  [[nodiscard]] ByteView user_key() const noexcept;

 private:
  static constexpr std::size_t InlineCapacity = 200;

  LookupKey() noexcept;

  [[nodiscard]] std::byte* data() noexcept;
  [[nodiscard]] const std::byte* data() const noexcept;
  void ResetToCanonicalEmpty() noexcept;

  std::array<std::byte, InlineCapacity> inline_storage_{};
  std::unique_ptr<std::byte[]> heap_storage_;
  std::size_t encoded_size_ = 0;
  std::size_t internal_key_offset_ = 0;
};

class InternalKey final {
 public:
  [[nodiscard]] static Result<InternalKey> Create(ByteView user_key, SequenceNumber sequence,
                                                  ValueKind kind);
  [[nodiscard]] static Result<InternalKey> Decode(ByteView encoded);

  InternalKey(const InternalKey&) = default;
  InternalKey& operator=(const InternalKey&) = default;
  InternalKey(InternalKey&&) noexcept = default;
  InternalKey& operator=(InternalKey&&) noexcept = default;
  ~InternalKey() = default;

  [[nodiscard]] ByteView encoded() const noexcept { return encoded_; }
  [[nodiscard]] ByteView user_key() const noexcept;
  [[nodiscard]] SequenceNumber sequence() const noexcept { return sequence_; }
  [[nodiscard]] ValueKind kind() const noexcept { return kind_; }

 private:
  InternalKey(std::vector<std::byte> encoded, SequenceNumber sequence, ValueKind kind)
      : encoded_(std::move(encoded)), sequence_(sequence), kind_(kind) {}

  std::vector<std::byte> encoded_;
  SequenceNumber sequence_;
  ValueKind kind_;
};

class InternalKeyComparator final : public Comparator {
 public:
  explicit InternalKeyComparator(const Comparator& user_comparator)
      : user_comparator_(user_comparator) {}
  InternalKeyComparator(Comparator&&) = delete;
  InternalKeyComparator(const Comparator&&) = delete;

  [[nodiscard]] int Compare(ByteView left, ByteView right) const noexcept override;
  [[nodiscard]] int Compare(const InternalKey& left, const InternalKey& right) const noexcept {
    return Compare(left.encoded(), right.encoded());
  }

  [[nodiscard]] std::string_view Name() const noexcept override;
  void FindShortestSeparator(std::vector<std::byte>& start, ByteView limit) const override;
  void FindShortSuccessor(std::vector<std::byte>& key) const override;

  [[nodiscard]] const Comparator& user_comparator() const noexcept { return user_comparator_; }

 private:
  const Comparator& user_comparator_;
};

}  // namespace modern_leveldb

#endif  // MODERN_LEVELDB_FORMAT_INTERNAL_KEY_H_
