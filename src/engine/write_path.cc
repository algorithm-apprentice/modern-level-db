#include "engine/write_path.h"

#include <cstddef>
#include <expected>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>

#include "modern_leveldb/base/bytes.h"

namespace modern_leveldb {
namespace {

// A group may grow to this size, or by this much when its first batch is at
// most this large, amortizing small writes without an unbounded group.
constexpr std::size_t MaximumGroupSize = std::size_t{1} << 20U;
constexpr std::size_t SmallBatchGrowth = std::size_t{128} << 10U;

Status PersistGroup(const EncodedWriteBatch& group, bool sync, WalWriter& log) {
    const Status appended = log.AddRecord(group.encoded());
    if (!appended.has_value()) {
        return appended;
    }
    if (sync) {
        const Status synced = log.Sync();
        if (!synced.has_value()) {
            return synced;
        }
    }
    return {};
}

}  // namespace

void InsertBatch(WriteBatchReader& batch, MemTable& memtable) {
    while (const std::optional<WriteBatchEntry> entry = batch.Next()) {
        memtable.Add(entry->sequence, entry->kind, entry->key, entry->value);
    }
}

Status CommitGroup(const EncodedWriteBatch& group, bool sync, WalWriter& log, MemTable& memtable) {
    Status persisted = PersistGroup(group, sync, log);
    if (!persisted.has_value()) {
        return persisted;
    }
    // A prepared group's keys fit the memtable, and its sequences are unused.
    WriteBatchReader entries = WriteBatchReader::OpenTrusted(group);
    InsertBatch(entries, memtable);
    return {};
}

// GCOVR_EXCL_START: GCC emits duplicate constructor/destructor ABI clones
WriteQueue::LeaderGuard::LeaderGuard(WriteQueue& queue, std::unique_lock<std::mutex>& lock,
                                     const Writer& leader) noexcept
    : queue_(queue), lock_(lock), leader_(leader), last_(&leader) {}

WriteQueue::LeaderGuard::~LeaderGuard() {
    if (dismissed_) {
        return;
    }
    if (!lock_.owns_lock()) {
        lock_.lock();
    }
    queue_.Complete(leader_, last_, queue_.aborted_);
}
// GCOVR_EXCL_STOP

// GCOVR_EXCL_START: GCC emits duplicate constructor/destructor ABI clones
WriteQueue::SequenceGuard::SequenceGuard(EncodedWriteBatch& batch) noexcept
    : batch_(batch), sequence_(batch.sequence()) {}

WriteQueue::SequenceGuard::~SequenceGuard() { batch_.SetSequence(sequence_); }
// GCOVR_EXCL_STOP

// GCOVR_EXCL_START: GCC emits duplicate constructor ABI clones
WriteQueue::WriteQueue()
    : aborted_(std::make_shared<const Status>(std::unexpected(
          Error::Aborted("write aborted by an exception while its group committed")))) {}
// GCOVR_EXCL_STOP

std::size_t WriteQueue::GroupSizeLimit(std::size_t leader_size) noexcept {
    return leader_size <= SmallBatchGrowth ? leader_size + SmallBatchGrowth : MaximumGroupSize;
}

bool WriteQueue::CompatibleFollower(const Writer& leader, const Writer& follower) noexcept {
    if (follower.batch == nullptr) {
        return false;
    }
    return !follower.sync || leader.sync;
}

bool WriteQueue::TryAddFollowerSize(std::size_t& size, std::size_t follower_size,
                                    std::size_t maximum_size) noexcept {
    size += follower_size;
    return size <= maximum_size;
}

void WriteQueue::AppendFollowerBatch(EncodedWriteBatch*& group, EncodedWriteBatch& leader_batch,
                                     const EncodedWriteBatch& follower_batch) {
    if (group == &leader_batch) {
        // Whatever sequence the leader holds, scratch starts from zero.
        group_.Clear();
        group_.Append(leader_batch);
        group = &group_;
    }
    group->Append(follower_batch);
}

WriteQueue::Group WriteQueue::BuildGroup(Writer& leader) {
    EncodedWriteBatch* group = leader.batch;
    std::size_t size = leader.batch->encoded().size();
    const std::size_t maximum_size = GroupSizeLimit(size);
    Writer* last = &leader;
    for (auto next = std::next(writers_.begin()); next != writers_.end(); ++next) {
        Writer* follower = *next;
        // A forced writer prepares on its own, and a group that does not sync must
        // not take a write that asks for a sync.
        if (!CompatibleFollower(leader, *follower)) {
            break;
        }
        if (!TryAddFollowerSize(size, follower->batch->encoded().size(), maximum_size)) {
            break;
        }
        AppendFollowerBatch(group, *leader.batch, *follower->batch);
        last = follower;
    }
    return {.batch = group, .last = last};
}

void WriteQueue::Complete(const Writer& leader, const Writer* last,
                          const std::shared_ptr<const Status>& result) noexcept {
    while (true) {
        Writer* ready = writers_.front();
        writers_.pop_front();
        if (ready != &leader) {
            ready->result = result;
            ready->done = true;
            ready->woken.notify_one();
        }
        if (ready == last) {
            break;
        }
    }
    if (!writers_.empty()) {
        writers_.front()->woken.notify_one();
    }
}

}  // namespace modern_leveldb
