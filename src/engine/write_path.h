#ifndef MODERN_LEVELDB_ENGINE_WRITE_PATH_H_
#define MODERN_LEVELDB_ENGINE_WRITE_PATH_H_

#include <cassert>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <expected>
#include <memory>
#include <mutex>
#include <type_traits>

#include "format/internal_key.h"
#include "format/write_batch.h"
#include "memory/memtable.h"
#include "modern_leveldb/base/result.h"
#include "wal/wal_io.h"

namespace modern_leveldb {

// Adds the remaining entries of the batch to the memtable with their sequences
// and returns the memtable's first error.
[[nodiscard]] Status InsertBatch(WriteBatchReader& batch, MemTable& memtable);

// Adds the remaining entries of a trusted owned batch whose sequence interval
// is reserved and whose internal keys are unique in the memtable.
void InsertBatchTrusted(WriteBatchReader& batch, MemTable& memtable);

// Sets the valid owned group's sequence. Returns InvalidArgument, changing
// nothing, if an entry would take a sequence above MaxSequenceNumber.
[[nodiscard]] Status PrepareGroup(EncodedWriteBatch& group, SequenceNumber first_sequence);

// Appends a prepared group to the log, syncs the log if asked, and inserts the
// group into the memtable. Returns the log's first error, which the log writer
// keeps. Only one writer at a time may call it for a memtable.
[[nodiscard]] Status CommitGroup(const EncodedWriteBatch& group, bool sync, WalWriter& log,
                                 MemTable& memtable);

// Serializes leaders under the database mutex and amortizes WAL work by grouping
// queued followers. Stack writer records stay alive until completion wakes them.
// See docs/learning/07-writes-and-compaction.md.
class WriteQueue final {
public:
    WriteQueue();

    WriteQueue(const WriteQueue&) = delete;
    WriteQueue& operator=(const WriteQueue&) = delete;
    WriteQueue(WriteQueue&&) = delete;
    WriteQueue& operator=(WriteQueue&&) = delete;
    ~WriteQueue() = default;

    // Requires the lock on the database mutex. Waits until the batch has been
    // committed in a group or its writer is at the front. At the front, it
    // prepares, and if that succeeds, commits a group of its batch and the
    // batches of the writers queued behind it, which return the same status. A
    // failed prepare fails only this writer. If a step throws, the writers of the
    // group return Aborted and the exception propagates. Commit may change only
    // the batch's hidden sequence, which the queue restores before completion.
    template <typename Prepare, typename Commit>
    [[nodiscard]] Status Write(std::unique_lock<std::mutex>& lock, EncodedWriteBatch& batch,
                               bool sync, Prepare&& prepare, Commit&& commit);

    // Requires the lock. Queues a writer without a batch that, at the front,
    // calls prepare with force set and completes alone. It is a rotation barrier:
    // a write group ends before it, so the log cannot switch during that commit.
    template <typename Prepare>
    [[nodiscard]] Status Force(std::unique_lock<std::mutex>& lock, Prepare&& prepare);

    // Returns the number of queued writers, including the front one. Requires the
    // lock.
    [[nodiscard]] std::size_t size() const noexcept { return writers_.size(); }

private:
    struct Writer {
        EncodedWriteBatch* batch;
        bool sync;
        bool done = false;
        std::shared_ptr<const Status> result{};
        std::condition_variable woken{};
    };

    struct Group {
        EncodedWriteBatch* batch;
        Writer* last;
    };

    class LeaderGuard final {
    public:
        LeaderGuard(WriteQueue& queue, std::unique_lock<std::mutex>& lock,
                    const Writer& leader) noexcept;
        LeaderGuard(const LeaderGuard&) = delete;
        LeaderGuard& operator=(const LeaderGuard&) = delete;
        LeaderGuard(LeaderGuard&&) = delete;
        LeaderGuard& operator=(LeaderGuard&&) = delete;
        ~LeaderGuard();

        void TakeOn(const Writer* last) noexcept { last_ = last; }
        void Dismiss() noexcept { dismissed_ = true; }

    private:
        WriteQueue& queue_;
        std::unique_lock<std::mutex>& lock_;
        const Writer& leader_;
        const Writer* last_;
        bool dismissed_ = false;
    };

    class SequenceGuard final {
    public:
        explicit SequenceGuard(EncodedWriteBatch& batch) noexcept;
        SequenceGuard(const SequenceGuard&) = delete;
        SequenceGuard& operator=(const SequenceGuard&) = delete;
        ~SequenceGuard();

    private:
        EncodedWriteBatch& batch_;
        SequenceNumber sequence_;
        std::uint32_t count_;
        std::size_t size_;
    };

    template <typename Prepare, typename Commit>
    [[nodiscard]] Status Run(std::unique_lock<std::mutex>& lock, EncodedWriteBatch* batch,
                             bool sync, Prepare& prepare, Commit& commit);
    // Uses the leader directly until the first admitted follower requires group_
    // and returns the selected batch with the group's last writer.
    [[nodiscard]] Group BuildGroup(Writer& leader);
    // Removes the writers from the front through `last`, gives them the result,
    // and wakes the next writer.
    void Complete(const Writer& leader, const Writer* last,
                  const std::shared_ptr<const Status>& result) noexcept;

    std::deque<Writer*> writers_;
    EncodedWriteBatch group_;
    // The result of a group whose commit threw, allocated in advance.
    std::shared_ptr<const Status> aborted_;
};

// GCOVR_EXCL_START: identical queue driver repeats for each callback type
template <typename Prepare, typename Commit>
Status WriteQueue::Write(std::unique_lock<std::mutex>& lock, EncodedWriteBatch& batch, bool sync,
                         Prepare&& prepare, Commit&& commit) {
    return Run(lock, &batch, sync, prepare, commit);
}

template <typename Prepare>
Status WriteQueue::Force(std::unique_lock<std::mutex>& lock, Prepare&& prepare) {
    auto unreachable_commit = [](std::unique_lock<std::mutex>&, EncodedWriteBatch&,
                                 bool) -> Status {
        assert(false);
        return std::unexpected(Error::Aborted("a forced writer unexpectedly reached commit"));
    };
    return Run(lock, nullptr, false, prepare, unreachable_commit);
}

template <typename Prepare, typename Commit>
Status WriteQueue::Run(std::unique_lock<std::mutex>& lock, EncodedWriteBatch* batch, bool sync,
                       Prepare& prepare, Commit& commit) {
    static_assert(std::is_nothrow_move_assignable_v<Status>);
    assert(lock.owns_lock());
    Writer writer{.batch = batch, .sync = sync};
    writers_.push_back(&writer);
    writer.woken.wait(lock, [&] { return writer.done || writers_.front() == &writer; });
    if (writer.done) {
        assert(writer.result != nullptr);
        return *writer.result;
    }

    LeaderGuard guard(*this, lock, writer);
    const Status prepared = prepare(lock, batch == nullptr);
    assert(lock.owns_lock());
    // A forced writer only prepares.
    if (!prepared.has_value() || batch == nullptr) {
        guard.Dismiss();
        Complete(writer, &writer, nullptr);
        return prepared;
    }

    const Group group = BuildGroup(writer);
    if (group.last == &writer) {
        guard.TakeOn(group.last);
        Status result;
        {
            SequenceGuard restore(*group.batch);
            result = commit(lock, *group.batch, writer.sync);
        }
        assert(lock.owns_lock());
        guard.Dismiss();
        Complete(writer, group.last, nullptr);
        return result;
    }

    // Allocate before commit so every taken-on writer receives a status without
    // an allocation after the group may have reached the WAL.
    const auto result = std::make_shared<Status>();
    guard.TakeOn(group.last);
    {
        SequenceGuard restore(*group.batch);
        *result = commit(lock, *group.batch, writer.sync);
    }
    assert(lock.owns_lock());
    guard.Dismiss();
    Complete(writer, group.last, result);
    return *result;
}
// GCOVR_EXCL_STOP

}  // namespace modern_leveldb

#endif  // MODERN_LEVELDB_ENGINE_WRITE_PATH_H_
