#ifndef MODERN_LEVELDB_FORMAT_WAL_FORMAT_H_
#define MODERN_LEVELDB_FORMAT_WAL_FORMAT_H_

#include <array>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <optional>

#include "modern_leveldb/base/bytes.h"
#include "modern_leveldb/base/result.h"

namespace modern_leveldb {

inline constexpr std::size_t WalBlockSize = 32U * 1'024U;
inline constexpr std::size_t WalHeaderSize = 7;

enum class WalRecordType : std::uint8_t {
  Zero = 0,
  Full = 1,
  First = 2,
  Middle = 3,
  Last = 4,
};

struct WalFragment {
  std::size_t padding_before;
  std::array<std::byte, WalHeaderSize> header;
  ByteView payload;
};

class WalFragmenter final {
 public:
  class Cursor final {
   public:
    Cursor(const Cursor&) = delete;
    Cursor& operator=(const Cursor&) = delete;
    Cursor(Cursor&&) = delete;
    Cursor& operator=(Cursor&&) = delete;
    ~Cursor() = default;

    [[nodiscard]] std::optional<WalFragment> Next() noexcept;

   private:
    friend class WalFragmenter;

    Cursor(std::size_t& block_offset, const std::array<std::uint32_t, 4>& type_crc,
           ByteView logical_record) noexcept
        : block_offset_(&block_offset), type_crc_(&type_crc), remaining_(logical_record) {}

    std::size_t* block_offset_;
    const std::array<std::uint32_t, 4>* type_crc_;
    ByteView remaining_;
    bool begin_ = true;
    bool done_ = false;
  };

  explicit WalFragmenter(std::uint64_t initial_file_size = 0) noexcept;

  WalFragmenter(const WalFragmenter&) = delete;
  WalFragmenter& operator=(const WalFragmenter&) = delete;
  WalFragmenter(WalFragmenter&&) = delete;
  WalFragmenter& operator=(WalFragmenter&&) = delete;

  [[nodiscard]] Cursor Fragment(ByteView logical_record) noexcept {
    return Cursor(block_offset_, type_crc_, logical_record);
  }
  [[nodiscard]] std::size_t block_offset() const noexcept { return block_offset_; }

 private:
  std::size_t block_offset_;
  std::array<std::uint32_t, 4> type_crc_{};
};

enum class WalDecodeKind {
  Fragment,
  EndOfBlock,
};

struct WalDecodeOutcome {
  WalDecodeKind kind;
  WalRecordType type;
  ByteView payload;
  std::size_t encoded_size;
};

enum class WalRecoveryAction {
  SkipPhysicalRecord,
  DropBlock,
};

enum class WalDecodeFailure {
  TruncatedHeader,
  TruncatedPayload,
  PayloadTooLarge,
  ChecksumMismatch,
  UnknownType,
};

struct WalDecodeError {
  Error error;
  WalDecodeFailure failure;
  WalRecoveryAction recovery;
  std::size_t encoded_size;
};

using WalDecodeResult = std::expected<WalDecodeOutcome, WalDecodeError>;

[[nodiscard]] WalDecodeResult DecodeWalFragment(ByteView encoded, bool verify_checksum = true);

}  // namespace modern_leveldb

#endif  // MODERN_LEVELDB_FORMAT_WAL_FORMAT_H_
