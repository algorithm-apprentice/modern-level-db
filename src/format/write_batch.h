#ifndef MODERN_LEVELDB_FORMAT_WRITE_BATCH_H_
#define MODERN_LEVELDB_FORMAT_WRITE_BATCH_H_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

#include "format/internal_key.h"
#include "modern_leveldb/base/bytes.h"
#include "modern_leveldb/base/result.h"

namespace modern_leveldb {

inline constexpr std::size_t WriteBatchHeaderSize = 12;

struct WriteBatchEntry {
  SequenceNumber sequence;
  ValueKind kind;
  ByteView key;
  ByteView value;
};

class WriteBatchReader final {
 public:
  [[nodiscard]] static Result<WriteBatchReader> Open(ByteView encoded);

  WriteBatchReader(const WriteBatchReader&) = delete;
  WriteBatchReader& operator=(const WriteBatchReader&) = delete;
  WriteBatchReader(WriteBatchReader&&) noexcept = default;
  WriteBatchReader& operator=(WriteBatchReader&&) noexcept = default;
  ~WriteBatchReader() = default;

  [[nodiscard]] SequenceNumber sequence() const noexcept { return sequence_; }
  [[nodiscard]] std::uint32_t count() const noexcept { return count_; }
  [[nodiscard]] std::optional<WriteBatchEntry> Next() noexcept;

 private:
  WriteBatchReader(ByteView records, SequenceNumber sequence, std::uint32_t count) noexcept
      : remaining_(records), sequence_(sequence), count_(count) {}

  ByteView remaining_;
  SequenceNumber sequence_;
  std::uint32_t count_;
  std::uint32_t index_ = 0;
};

class WriteBatch final {
 public:
  WriteBatch();

  WriteBatch(const WriteBatch&) = default;
  WriteBatch& operator=(const WriteBatch&) = default;
  WriteBatch(WriteBatch&& source);
  WriteBatch& operator=(WriteBatch&& source);
  ~WriteBatch() = default;

  [[nodiscard]] Status Put(ByteView key, ByteView value);
  [[nodiscard]] Status Delete(ByteView key);
  [[nodiscard]] Status Append(const WriteBatch& source);
  [[nodiscard]] Status SetSequence(SequenceNumber sequence);
  void Clear() noexcept;

  [[nodiscard]] SequenceNumber sequence() const noexcept;
  [[nodiscard]] std::uint32_t count() const noexcept;
  [[nodiscard]] ByteView encoded() const noexcept { return encoded_; }

 private:
  [[nodiscard]] Status ValidateAdditionalRecords(std::uint32_t additional) const;
  void SetCount(std::uint32_t count) noexcept;

  std::vector<std::byte> encoded_;
};

}  // namespace modern_leveldb

#endif  // MODERN_LEVELDB_FORMAT_WRITE_BATCH_H_
