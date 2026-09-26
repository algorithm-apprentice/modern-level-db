#include "engine/write_path.h"

#include <cassert>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <expected>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>

#include "modern_leveldb/base/bytes.h"

namespace modern_leveldb {
namespace {

// A group may grow to this size, or by this much when its first batch is at
// most this large, as in LevelDB.
constexpr std::size_t MaximumGroupSize = std::size_t{1} << 20U;
constexpr std::size_t SmallBatchGrowth = std::size_t{128} << 10U;

// The longest user key whose internal key the memtable accepts.
constexpr std::size_t MaximumKeySize =
    std::numeric_limits<std::uint32_t>::max() - InternalKeyTrailerSize;

// GCOVR_EXCL_START: only keys larger than 4 GiB reach this function
std::unexpected<Error> KeyTooLong() {
  return std::unexpected(Error::InvalidArgument("write batch key is too long for the memtable"));
}
// GCOVR_EXCL_STOP

// Appends a batch to a group that is numbered from zero, which fails only if
// the count overflows: the group's first batch starts an empty group, and the
// size limit keeps the count of a larger group small.
void AppendToGroup(EncodedWriteBatch& group, const EncodedWriteBatch& batch) {
  const Status appended = group.Append(batch);
  assert(appended.has_value());
  static_cast<void>(appended);
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

Status PrepareGroup(EncodedWriteBatch& group, SequenceNumber first_sequence) {
  WriteBatchReader entries = WriteBatchReader::Open(group.encoded()).value();
  while (const std::optional<WriteBatchEntry> entry = entries.Next()) {
    if (entry->key.size() > MaximumKeySize) {  // GCOVR_EXCL_BR_WITHOUT_HIT: 1/2 needs over 4 GiB
      return KeyTooLong();                     // GCOVR_EXCL_LINE: needs a key over 4 GiB
    }
  }
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
  WriteBatchReader entries = WriteBatchReader::Open(group.encoded()).value();
  const Status inserted = InsertBatch(entries, memtable);
  assert(inserted.has_value());
  static_cast<void>(inserted);
  return {};
}

struct WriteQueue::Writer {
  const EncodedWriteBatch* batch;
  bool sync;
  bool done = false;
  std::shared_ptr<const Status> result{};
  std::condition_variable woken{};
};

// Completes the writers that the front writer has taken on if a step throws.
class WriteQueue::LeaderGuard final {
 public:
  LeaderGuard(WriteQueue& queue, std::unique_lock<std::mutex>& lock, const Writer& leader) noexcept
      : queue_(queue), lock_(lock), leader_(leader), last_(&leader) {}
  LeaderGuard(const LeaderGuard&) = delete;
  LeaderGuard& operator=(const LeaderGuard&) = delete;
  LeaderGuard(LeaderGuard&&) = delete;
  LeaderGuard& operator=(LeaderGuard&&) = delete;
  ~LeaderGuard() {
    if (dismissed_) {
      return;
    }
    if (!lock_.owns_lock()) {
      lock_.lock();
    }
    queue_.Complete(leader_, last_, queue_.aborted_);
  }

  void TakeOn(const Writer* last) noexcept { last_ = last; }
  void Dismiss() noexcept { dismissed_ = true; }

 private:
  WriteQueue& queue_;
  std::unique_lock<std::mutex>& lock_;
  const Writer& leader_;
  const Writer* last_;
  bool dismissed_ = false;
};

WriteQueue::WriteQueue(Prepare prepare, Commit commit)
    : prepare_(std::move(prepare)),
      commit_(std::move(commit)),
      aborted_(std::make_shared<const Status>(std::unexpected(
          Error::Aborted("write aborted by an exception while its group committed")))) {}

Status WriteQueue::Write(std::unique_lock<std::mutex>& lock, const EncodedWriteBatch& batch,
                         bool sync) {
  return Run(lock, &batch, sync);
}

Status WriteQueue::Force(std::unique_lock<std::mutex>& lock) { return Run(lock, nullptr, false); }

Status WriteQueue::Run(std::unique_lock<std::mutex>& lock, const EncodedWriteBatch* batch,
                       bool sync) {
  assert(lock.owns_lock());
  Writer writer{.batch = batch, .sync = sync};
  writers_.push_back(&writer);
  writer.woken.wait(lock, [&] { return writer.done || writers_.front() == &writer; });
  if (writer.done) {
    return *writer.result;
  }

  LeaderGuard guard(*this, lock, writer);
  const Status prepared = prepare_(lock, batch == nullptr);
  // A forced writer only prepares.
  if (!prepared.has_value() || batch == nullptr) {
    guard.Dismiss();
    Complete(writer, &writer, nullptr);
    return prepared;
  }
  const Writer* last = BuildGroup(writer);
  // Allocated first, so that a committed group always gets its status.
  const auto result = std::make_shared<Status>();
  // A group whose commit throws may be in the log, so its writers are aborted.
  guard.TakeOn(last);
  *result = commit_(lock, group_, writer.sync);
  guard.Dismiss();
  Complete(writer, last, result);
  return *result;
}

WriteQueue::Writer* WriteQueue::BuildGroup(const Writer& leader) {
  // Whatever sequences the callers' batches hold, the group starts from zero.
  group_.Clear();
  AppendToGroup(group_, *leader.batch);
  std::size_t size = group_.encoded().size();
  const std::size_t maximum_size =
      size <= SmallBatchGrowth ? size + SmallBatchGrowth : MaximumGroupSize;
  Writer* last = writers_.front();
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
    AppendToGroup(group_, *follower->batch);
    last = follower;
  }
  return last;
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
