#ifndef MODERN_LEVELDB_WAL_WAL_IO_H_
#define MODERN_LEVELDB_WAL_WAL_IO_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string_view>
#include <variant>
#include <vector>

#include "format/wal_format.h"
#include "modern_leveldb/base/bytes.h"
#include "modern_leveldb/base/result.h"
#include "platform/file_system.h"

namespace modern_leveldb {

// Owns a serialized WAL stream. AddRecord writes all fragments and Flushes;
// only Sync requests durable contents. The first ordinary I/O error poisons
// later record/sync calls, but Close still releases the file.
class WalWriter final {
public:
    // Reopening a partial block pads to the next boundary before a new record,
    // keeping a crash tail from being combined with later fragments.
    explicit WalWriter(std::unique_ptr<WritableFile> file, std::uint64_t initial_file_size = 0);

    WalWriter(const WalWriter&) = delete;
    WalWriter& operator=(const WalWriter&) = delete;
    WalWriter(WalWriter&&) = delete;
    WalWriter& operator=(WalWriter&&) = delete;
    ~WalWriter() = default;

    [[nodiscard]] Status AddRecord(ByteView logical_record);
    [[nodiscard]] Status Sync();
    [[nodiscard]] Status Close();

private:
    [[nodiscard]] Status CheckUsable(std::string_view operation) const;
    [[nodiscard]] Status RememberError(Status status);
    [[nodiscard]] Status Append(ByteView data);

    std::unique_ptr<WritableFile> file_;
    WalFragmenter fragmenter_;
    std::optional<Error> first_error_;
    std::size_t reopen_padding_ = 0;
    bool closed_ = false;
};

// data borrows the reader's block/scratch buffer. Consume or copy before the next
// ReadNext or reader destruction; retaining this event does not retain its bytes.
struct WalLogicalRecord {
    ByteView data;
    std::uint64_t offset;
};

struct WalCorruption {
    Error error;
    std::uint64_t dropped_bytes;
    std::uint64_t offset;
};

using WalReadEvent = std::variant<WalLogicalRecord, WalCorruption>;
// Record/corruption are events; empty optional is EOF (including a truncated
// crash tail); failed Result is terminal I/O/handle failure. WAL replay may skip
// corruption, while MANIFEST replay rejects it at the authoritative-metadata boundary.
using WalReadResult = Result<std::optional<WalReadEvent>>;

// Reassembles complete logical records, never exposing a partial batch for replay.
// Calls and buffer use need external serialization.
class WalReader final {
public:
    explicit WalReader(std::unique_ptr<SequentialFile> file);

    WalReader(const WalReader&) = delete;
    WalReader& operator=(const WalReader&) = delete;
    WalReader(WalReader&&) = delete;
    WalReader& operator=(WalReader&&) = delete;
    ~WalReader() = default;

    [[nodiscard]] WalReadResult ReadNext();

private:
    struct PhysicalFragment {
        WalRecordType type;
        ByteView payload;
        std::uint64_t offset;
        std::size_t block_position;
    };
    using PhysicalRead = std::variant<PhysicalFragment, WalCorruption>;

    [[nodiscard]] Result<std::optional<PhysicalRead>> ReadPhysical();
    [[nodiscard]] Result<bool> FillBlock();
    [[nodiscard]] bool HasPartialPayload() const noexcept {
        return in_fragmented_record_ && !scratch_.empty();
    }
    [[nodiscard]] WalReadResult AbandonPartial(std::string_view reason);
    void ClearPartial() noexcept;

    std::unique_ptr<SequentialFile> file_;
    std::array<std::byte, WalBlockSize> block_{};
    std::size_t block_size_ = 0;
    std::size_t block_position_ = 0;
    std::uint64_t block_start_offset_ = 0;
    std::vector<std::byte> scratch_;
    std::optional<Error> terminal_error_;
    std::uint64_t partial_record_offset_ = 0;
    bool eof_seen_ = false;
    bool in_fragmented_record_ = false;
};

}  // namespace modern_leveldb

#endif  // MODERN_LEVELDB_WAL_WAL_IO_H_
