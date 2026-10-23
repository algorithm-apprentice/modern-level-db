#include "engine/write_buffer_state.h"

#include <algorithm>
#include <utility>

namespace modern_leveldb {

void WriteBufferState::InstallRecovered(std::unique_ptr<WalWriter> log, std::uint64_t log_number,
                                        std::shared_ptr<MemTable> mutable_table) {
    InstallLogGeneration(std::move(log), log_number);
    InstallMutableGeneration(std::move(mutable_table));
}

void WriteBufferState::InstallLogGeneration(std::unique_ptr<WalWriter> log,
                                            std::uint64_t log_number) {
    log_ = std::move(log);
    log_number_ = log_number;
}

void WriteBufferState::InstallMutableGeneration(std::shared_ptr<MemTable> mutable_table) {
    mutable_ = std::move(mutable_table);
    published_mutable_bytes_ = mutable_->memory_usage();
}

void WriteBufferState::InstallRotation(std::unique_ptr<WalWriter> log, std::uint64_t log_number,
                                       std::shared_ptr<MemTable> mutable_table) {
    InstallLogGeneration(std::move(log), log_number);
    PromoteMutableToImmutable();
    InstallMutableGeneration(std::move(mutable_table));
}

void WriteBufferState::PromoteMutableToImmutable() noexcept {
    immutable_ = std::move(mutable_);
    has_immutable_.store(true, std::memory_order_release);
}

void WriteBufferState::ClearImmutable() {
    RetireImmutable();
    has_immutable_.store(false, std::memory_order_relaxed);
}

void WriteBufferState::RetireImmutable() {
    if (immutable_->read_pins_ != 0) {
        read_pinned_.push_back(std::move(immutable_));
    } else {
        immutable_.reset();
    }
}

void WriteBufferState::ReleaseReadPinnedMemtables() {
    std::erase_if(read_pinned_, [](const std::shared_ptr<const MemTable>& table) {
        return table->read_pins_ == 0;
    });
}

}  // namespace modern_leveldb
