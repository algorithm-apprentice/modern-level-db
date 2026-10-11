#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <utility>

#include "engine/database.h"
#include "metadata/filenames.h"
#include "platform/file_system.h"
#include "wal/wal_io.h"

namespace modern_leveldb {

Status DatabaseEngine::Write(EncodedWriteBatch& batch, bool sync) {
    std::unique_lock lock(mutex_);
    return write_queue_.Write(
        lock, batch, sync,
        [this](std::unique_lock<std::mutex>& owned_lock, bool force) {
            return MakeRoomForWrite(owned_lock, force);
        },
        [this](std::unique_lock<std::mutex>& owned_lock, EncodedWriteBatch& group,
               bool group_sync) { return CommitWrite(owned_lock, group, group_sync); });
}

Status DatabaseEngine::MakeRoomForWrite(std::unique_lock<std::mutex>& lock, bool force) {
    bool allow_delay = !force;
    while (true) {
        const std::size_t level0_files = versions_->current_raw()->files(0).size();
        if (sticky_error_.has_value()) {
            return StickyError();
        }
        if (allow_delay && level0_files >= WriteDelayLevel0FileCountThreshold) {
            // Near the stop, each write waits a little once rather than a few
            // writes waiting long, and the compaction gets the time.
            lock.unlock();
            static_cast<void>(clock_->SleepFor(std::chrono::milliseconds(1)));
            lock.lock();
            allow_delay = false;
            continue;
        }
        if (!force && memtable_->memory_usage() <= write_buffer_size_) {
            return {};
        }
        if (immutable_ != nullptr || level0_files >= WriteStallLevel0FileCountThreshold) {
            background_finished_.wait(lock);
            continue;
        }
        const Status switched = SwitchMemTable();
        if (!switched.has_value()) {
            return switched;
        }
        force = false;
    }
}

Status DatabaseEngine::SwitchMemTable() {
    const std::uint64_t number = versions_->NewFileNumber();
    const std::filesystem::path path = LogFileName(directory_, number);
    Result<std::unique_ptr<WritableFile>> file = file_system_->OpenWritable(path);
    if (!file.has_value()) {
        return std::unexpected(std::move(file).error());
    }
    // Allocate replacement owners before swapping live log/memtable state.
    // The file number and on-disk name may already have been reserved/created.
    auto new_log = std::make_unique<WalWriter>(std::move(*file));
    auto new_memtable = std::make_shared<MemTable>(comparator_.user_comparator());
    if (sync_wal_creation_) {
        // The flush of the old memtable makes this the oldest log that recovery
        // replays, so its directory entry is durable before it takes writes.
        const Status synced = file_system_->SyncDirectory(directory_);
        if (!synced.has_value()) {
            return synced;
        }
    }
    Status closed;
    try {
        closed = log_->Close();
    } catch (...) {
        RecordStickyError(Error::Aborted("old WAL close threw during rotation"));
        throw;
    }
    if (!closed.has_value()) {
        RecordStickyError(closed.error());
    }
    log_ = std::move(new_log);
    log_number_ = number;
    immutable_ = std::move(memtable_);
    has_immutable_.store(true, std::memory_order_release);
    memtable_ = std::move(new_memtable);
    published_mutable_memtable_bytes_ = memtable_->memory_usage();
    MaybeScheduleBackgroundWork();
    return {};
}

Status DatabaseEngine::CommitWrite(std::unique_lock<std::mutex>& lock, EncodedWriteBatch& group,
                                   bool sync) {
    const SequenceNumber first = versions_->last_sequence() + 1;
    group.SetSequence(first);
    const std::uint32_t count = group.count();
    // Only the front writer switches the log and the memtable, so they stay.
    WalWriter& log = *log_;
    MemTable& memtable = *memtable_;
    lock.unlock();
    Status committed;
    try {
        committed = CommitGroup(group, sync, log, memtable);
    } catch (...) {
        lock.lock();
        RecordStickyError(Error::Aborted("a write stopped with an exception while it committed"));
        throw;
    }
    lock.lock();
    if (!committed.has_value()) {
        RecordStickyError(committed.error());
        return committed;
    }
    published_mutable_memtable_bytes_ = memtable.memory_usage();
    // Publish only after the complete group is in the memtable. Concurrent
    // readers use the previous sequence and therefore hide partially inserted
    // newer entries. An empty group leaves the sequence unchanged.
    versions_->SetLastSequence(first + count - 1);
    return {};
}

Status DatabaseEngine::FlushMemTable() {
    std::unique_lock lock(mutex_);
    auto prepare = [this](std::unique_lock<std::mutex>& owned_lock, bool force) {
        return MakeRoomForWrite(owned_lock, force);
    };
    const Status forced = write_queue_.Force(lock, prepare);
    if (!forced.has_value()) {
        return forced;
    }
    // The forced switch made the immutable memtable, and the lock stayed held.
    const std::shared_ptr<const MemTable> flushing = immutable_;
    background_finished_.wait(lock,
                              [&] { return immutable_ != flushing || sticky_error_.has_value(); });
    if (sticky_error_.has_value()) {
        return StickyError();
    }
    return {};
}

}  // namespace modern_leveldb
