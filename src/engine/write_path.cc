#include "engine/write_path.h"

#include <cassert>
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

// Scratch starts at sequence zero; the group-size limit also bounds its record count.
void AppendToGroup(EncodedWriteBatch& group, const EncodedWriteBatch& batch) {
    group.AppendTrusted(batch);
}

}  // namespace

Status InsertBatch(WriteBatchReader& batch, MemTable& memtable) {
    while (const std::optional<WriteBatchEntry> entry = batch.Next()) {
        const Status added = memtable.Add(entry->sequence, entry->kind, entry->key, entry->value);
        if (!added.has_value()) {
            return added;
        }
    }
    return {};
}

void InsertBatchTrusted(WriteBatchReader& batch, MemTable& memtable) {
    while (const std::optional<WriteBatchEntry> entry = batch.Next()) {
        memtable.AddTrusted(entry->sequence, entry->kind, entry->key, entry->value);
    }
}

Status PrepareGroup(EncodedWriteBatch& group, SequenceNumber first_sequence) {
    return group.SetSequence(first_sequence);
}

Status CommitGroup(const EncodedWriteBatch& group, bool sync, WalWriter& log, MemTable& memtable) {
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
    // A prepared group's keys fit the memtable, and its sequences are unused.
    WriteBatchReader entries = WriteBatchReader::OpenTrusted(group);
    InsertBatchTrusted(entries, memtable);
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
    : batch_(batch),
      sequence_(batch.sequence()),
      count_(batch.count()),
      size_(batch.encoded().size()) {}

WriteQueue::SequenceGuard::~SequenceGuard() {
    assert(batch_.count() == count_);
    assert(batch_.encoded().size() == size_);
    const Status restored = batch_.SetSequence(sequence_);
    assert(restored.has_value());
    static_cast<void>(restored);
}
// GCOVR_EXCL_STOP

// GCOVR_EXCL_START: GCC emits duplicate constructor ABI clones
WriteQueue::WriteQueue()
    : aborted_(std::make_shared<const Status>(std::unexpected(
          Error::Aborted("write aborted by an exception while its group committed")))) {}
// GCOVR_EXCL_STOP

WriteQueue::Group WriteQueue::BuildGroup(Writer& leader) {
    EncodedWriteBatch* group = leader.batch;
    std::size_t size = leader.batch->encoded().size();
    const std::size_t maximum_size =
        size <= SmallBatchGrowth ? size + SmallBatchGrowth : MaximumGroupSize;
    Writer* last = &leader;
    for (auto next = std::next(writers_.begin()); next != writers_.end(); ++next) {
        Writer* follower = *next;
        // A forced writer prepares on its own, and a group that does not sync must
        // not take a write that asks for a sync.
        if (follower->batch == nullptr || (follower->sync && !leader.sync)) {
            break;
        }
        size += follower->batch->encoded().size();
        if (size > maximum_size) {
            break;
        }
        if (group == leader.batch) {
            // Whatever sequence the leader holds, scratch starts from zero.
            group_.Clear();
            AppendToGroup(group_, *leader.batch);
            group = &group_;
        }
        AppendToGroup(group_, *follower->batch);
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
            assert(result != nullptr);
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
