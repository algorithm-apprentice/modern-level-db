#include "engine/write_buffer_state.h"

#include <algorithm>
#include <utility>

namespace modern_leveldb {

void WriteBufferState::InstallRecovered(std::unique_ptr<WalWriter> log, std::uint64_t log_number,
                                        std::shared_ptr<MemTable> mutable_table) {
    log_ = std::move(log);
    log_number_ = log_number;
    mutable_ = std::move(mutable_table);
    published_mutable_bytes_ = mutable_->memory_usage();
}

void WriteBufferState::InstallRotation(std::unique_ptr<WalWriter> log, std::uint64_t log_number,
                                       std::shared_ptr<MemTable> mutable_table) {
    log_ = std::move(log);
    log_number_ = log_number;
    immutable_ = std::move(mutable_);
    has_immutable_.store(true, std::memory_order_release);
    mutable_ = std::move(mutable_table);
    published_mutable_bytes_ = mutable_->memory_usage();
}

WalWriter& WriteBufferState::log() noexcept { return *log_; }

MemTable& WriteBufferState::mutable_table() noexcept { return *mutable_; }

void WriteBufferState::PublishMutableMemoryUsage() noexcept {
    published_mutable_bytes_ = mutable_table().memory_usage();
}

void WriteBufferState::ClearImmutable() {
    if (immutable_->read_pins_ != 0) {
        read_pinned_.push_back(std::move(immutable_));
    } else {
        immutable_.reset();
    }
    has_immutable_.store(false, std::memory_order_relaxed);
}

void WriteBufferState::ReleaseReadPinnedMemtables() {
    std::erase_if(read_pinned_, [](const std::shared_ptr<const MemTable>& table) {
        return table->read_pins_ == 0;
    });
}

}  // namespace modern_leveldb
