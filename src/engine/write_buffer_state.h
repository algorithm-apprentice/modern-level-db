#ifndef MODERN_LEVELDB_ENGINE_WRITE_BUFFER_STATE_H_
#define MODERN_LEVELDB_ENGINE_WRITE_BUFFER_STATE_H_

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "memory/memtable.h"
#include "wal/wal_io.h"

namespace modern_leveldb {

// Owns the active WAL/mutable MemTable generation and the one immutable
// generation awaiting flush. All methods except HasImmutableForCompaction
// require the database mutex.
class WriteBufferState final {
public:
    WriteBufferState() = default;

    WriteBufferState(const WriteBufferState&) = delete;
    WriteBufferState& operator=(const WriteBufferState&) = delete;
    WriteBufferState(WriteBufferState&&) = delete;
    WriteBufferState& operator=(WriteBufferState&&) = delete;
    ~WriteBufferState() = default;

    void InstallRecovered(std::unique_ptr<WalWriter> log, std::uint64_t log_number,
                          std::shared_ptr<MemTable> mutable_table);
    // Requires the current WAL Close to have returned and no immutable table.
    void InstallRotation(std::unique_ptr<WalWriter> log, std::uint64_t log_number,
                         std::shared_ptr<MemTable> mutable_table);

    [[nodiscard]] bool has_log() const noexcept { return log_ != nullptr; }
    [[nodiscard]] WalWriter& log() noexcept;
    [[nodiscard]] std::uint64_t log_number() const noexcept { return log_number_; }
    [[nodiscard]] MemTable& mutable_table() noexcept;
    [[nodiscard]] const std::shared_ptr<MemTable>& mutable_owner() const noexcept {
        return mutable_;
    }
    [[nodiscard]] const std::shared_ptr<const MemTable>& immutable_owner() const noexcept {
        return immutable_;
    }
    [[nodiscard]] bool has_immutable() const noexcept { return immutable_ != nullptr; }
    [[nodiscard]] bool HasImmutableForCompaction() const noexcept {
        return has_immutable_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] std::size_t published_mutable_bytes() const noexcept {
        return published_mutable_bytes_;
    }
    void PublishMutableMemoryUsage() noexcept;

    // Requires an installed immutable table whose VersionEdit was committed.
    void ClearImmutable();
    void ReleaseReadPinnedMemtables();

private:
    void InstallLogGeneration(std::unique_ptr<WalWriter> log, std::uint64_t log_number);
    void InstallMutableGeneration(std::shared_ptr<MemTable> mutable_table);
    void PromoteMutableToImmutable() noexcept;
    void RetireImmutable();

    std::unique_ptr<WalWriter> log_;
    std::uint64_t log_number_ = 0;
    std::shared_ptr<MemTable> mutable_;
    std::size_t published_mutable_bytes_ = 0;
    std::shared_ptr<const MemTable> immutable_;
    std::vector<std::shared_ptr<const MemTable>> read_pinned_;
    std::atomic<bool> has_immutable_ = false;
};

}  // namespace modern_leveldb

#endif  // MODERN_LEVELDB_ENGINE_WRITE_BUFFER_STATE_H_
