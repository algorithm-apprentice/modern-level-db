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

class WalWriter final {
 public:
  explicit WalWriter(std::unique_ptr<WritableFile> file,
                     std::uint64_t initial_file_size = 0);

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

struct WalReaderOptions {
  std::uint64_t initial_offset = 0;
};

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
using WalReadResult = Result<std::optional<WalReadEvent>>;

class WalReader final {
 public:
  explicit WalReader(std::unique_ptr<SequentialFile> file,
                     WalReaderOptions options = {});

  WalReader(const WalReader&) = delete;
  WalReader& operator=(const WalReader&) = delete;
  WalReader(WalReader&&) = delete;
  WalReader& operator=(WalReader&&) = delete;
  ~WalReader() = default;

  [[nodiscard]] WalReadResult ReadNext();

 private:
  [[nodiscard]] Status Initialize();
  [[nodiscard]] Result<bool> FillBlock();
  [[nodiscard]] WalReadResult ReturnTerminal(Error error);
  [[nodiscard]] WalReadResult ReturnRecord(ByteView data,
                                           std::uint64_t offset);
  [[nodiscard]] WalReadResult ReturnCorruption(
      Error error, std::uint64_t dropped_bytes, std::uint64_t offset);
  void ClearPartial() noexcept;

  std::unique_ptr<SequentialFile> file_;
  const std::uint64_t initial_offset_;
  std::array<std::byte, WalBlockSize> block_{};
  std::size_t block_size_ = 0;
  std::size_t block_position_ = 0;
  std::uint64_t block_start_offset_ = 0;
  std::uint64_t next_block_offset_ = 0;
  std::vector<std::byte> scratch_;
  std::optional<WalLogicalRecord> pending_record_;
  std::optional<Error> terminal_error_;
  std::uint64_t partial_record_offset_ = 0;
  bool initialized_ = false;
  bool eof_seen_ = false;
  bool exhausted_ = false;
  bool resyncing_;
  bool in_fragmented_record_ = false;
};

}  // namespace modern_leveldb

#endif  // MODERN_LEVELDB_WAL_WAL_IO_H_
