#ifndef MODERN_LEVELDB_FORMAT_WRITE_BATCH_H_
#define MODERN_LEVELDB_FORMAT_WRITE_BATCH_H_

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>

#include "format/internal_key.h"
#include "modern_leveldb/base/bytes.h"
#include "modern_leveldb/base/result.h"

namespace modern_leveldb {

inline constexpr std::size_t WriteBatchHeaderSize = 12;

// fixed64(first_sequence) || fixed32(count) || ordered records. A Put record has
// a kind byte and length-prefixed key/value; Delete has only the key. Each record
// consumes the next sequence. Framing/checksums belong to the WAL, not this payload.
// See docs/learning/02-bytes-and-formats.md.
struct WriteBatchEntry {
    SequenceNumber sequence;
    ValueKind kind;
    ByteView key;
    ByteView value;
};

class EncodedWriteBatch;

class WriteBatchReader final {
public:
    // Validates the complete external representation before iteration: tags,
    // lengths, count, sequence range, and no trailing bytes. The reader and its
    // entries borrow the input, which must stay alive and unchanged.
    [[nodiscard]] static Result<WriteBatchReader> Open(ByteView encoded);
    // CommitGroup uses the owned batch's private encoding invariant instead of
    // rescanning disk bytes. The batch must remain alive and unchanged while read.
    [[nodiscard]] static WriteBatchReader OpenTrusted(const EncodedWriteBatch& batch) noexcept;

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

// Owns bytes whose mutators preserve a fully decodable batch. The write queue
// temporarily assigns its hidden sequence and restores it before returning.
class EncodedWriteBatch final {
public:
    EncodedWriteBatch();

    EncodedWriteBatch(const EncodedWriteBatch&) = default;
    EncodedWriteBatch& operator=(const EncodedWriteBatch&) = default;
    EncodedWriteBatch(EncodedWriteBatch&& source);
    EncodedWriteBatch& operator=(EncodedWriteBatch&& source);
    ~EncodedWriteBatch() = default;

    [[nodiscard]] Status Put(ByteView key, ByteView value);
    [[nodiscard]] Status Delete(ByteView key);
    [[nodiscard]] Status Append(const EncodedWriteBatch& source);
    [[nodiscard]] Status SetSequence(SequenceNumber sequence);
    void Clear() noexcept;

    [[nodiscard]] SequenceNumber sequence() const noexcept;
    [[nodiscard]] std::uint32_t count() const noexcept;
    [[nodiscard]] ByteView encoded() const noexcept { return AsBytes(encoded_); }

private:
    [[nodiscard]] Status AppendRecord(ValueKind kind, ByteView key, ByteView value);
    [[nodiscard]] Status ValidateAdditionalRecords(std::uint32_t additional) const;
    void SetCount(std::uint32_t count) noexcept;

    std::string encoded_;
};

}  // namespace modern_leveldb

#endif  // MODERN_LEVELDB_FORMAT_WRITE_BATCH_H_
