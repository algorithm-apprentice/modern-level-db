#include "engine/write_path.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "format/internal_key.h"
#include "format/write_batch.h"
#include "memory/memtable.h"
#include "modern_leveldb/base/bytes.h"
#include "modern_leveldb/base/comparator.h"
#include "modern_leveldb/base/result.h"
#include "support/memory_file_system.h"
#include "wal/wal_io.h"

namespace modern_leveldb {
namespace {

using test_support::MemoryFileSystem;

static_assert(!std::is_copy_constructible_v<WriteQueue>);
static_assert(!std::is_move_constructible_v<WriteQueue>);

WriteBatch Batch(std::initializer_list<std::pair<std::string_view, std::string_view>> puts) {
  WriteBatch batch;
  for (const auto& [key, value] : puts) {
    EXPECT_TRUE(batch.Put(AsBytes(key), AsBytes(value)).has_value());
  }
  return batch;
}

// The keys of a batch's entries, joined by commas.
std::string Keys(const WriteBatch& batch) {
  std::string keys;
  WriteBatchReader reader = WriteBatchReader::Open(batch.encoded()).value();
  while (const std::optional<WriteBatchEntry> entry = reader.Next()) {
    if (!keys.empty()) {
      keys += ",";
    }
    keys += std::string(AsStringView(entry->key));
  }
  return keys;
}

std::string Lookup(const MemTable& memtable, std::string_view key, SequenceNumber sequence) {
  const MemTableLookup found = memtable.Lookup(LookupKey::Create(AsBytes(key), sequence).value());
  if (found.kind == MemTableLookupKind::Missing) {
    return "<missing>";
  }
  if (found.kind == MemTableLookupKind::Deletion) {
    return "<deleted>";
  }
  return std::string(AsStringView(found.value));
}

TEST(InsertBatchTest, AddsEntriesWithTheirSequences) {
  WriteBatch batch;
  ASSERT_TRUE(batch.Put(AsBytes("a"), AsBytes("alpha")).has_value());
  ASSERT_TRUE(batch.Delete(AsBytes("b")).has_value());
  ASSERT_TRUE(batch.Put(AsBytes("a"), AsBytes("again")).has_value());
  ASSERT_TRUE(batch.SetSequence(10).has_value());
  MemTable memtable(BytewiseComparator());

  WriteBatchReader reader = WriteBatchReader::Open(batch.encoded()).value();
  ASSERT_TRUE(InsertBatch(reader, memtable).has_value());

  EXPECT_EQ(Lookup(memtable, "a", 10), "alpha");
  EXPECT_EQ(Lookup(memtable, "a", 12), "again");
  EXPECT_EQ(Lookup(memtable, "b", 11), "<deleted>");
  EXPECT_EQ(Lookup(memtable, "b", 10), "<missing>");

  // Inserting the same entries again is rejected.
  WriteBatchReader again = WriteBatchReader::Open(batch.encoded()).value();
  const Status duplicate = InsertBatch(again, memtable);
  ASSERT_FALSE(duplicate.has_value());
  EXPECT_EQ(duplicate.error().code(), ErrorCode::InvalidArgument);
}

TEST(PrepareGroupTest, SetsTheSequenceWithinItsRange) {
  WriteBatch group = Batch({{"a", "1"}, {"b", "2"}, {"c", "3"}});

  ASSERT_TRUE(PrepareGroup(group, 5).has_value());
  EXPECT_EQ(group.sequence(), 5U);
  ASSERT_TRUE(PrepareGroup(group, MaxSequenceNumber - 2).has_value());
  EXPECT_EQ(group.sequence(), MaxSequenceNumber - 2);

  const Status exhausted = PrepareGroup(group, MaxSequenceNumber - 1);
  ASSERT_FALSE(exhausted.has_value());
  EXPECT_EQ(exhausted.error().code(), ErrorCode::InvalidArgument);
  EXPECT_EQ(group.sequence(), MaxSequenceNumber - 2);

  WriteBatch empty;
  ASSERT_TRUE(PrepareGroup(empty, MaxSequenceNumber).has_value());
  EXPECT_FALSE(PrepareGroup(empty, MaxSequenceNumber + 1).has_value());
}

class CommitGroupTest : public testing::Test {
 protected:
  std::unique_ptr<WalWriter> OpenLog() {
    auto file = file_system_.OpenWritable(std::filesystem::path("db") / "000005.log");
    EXPECT_TRUE(file.has_value());
    return std::make_unique<WalWriter>(std::move(file).value());
  }

  std::vector<std::string> Operations(std::size_t start) const {
    return std::vector<std::string>(
        file_system_.operations().begin() + static_cast<std::ptrdiff_t>(start),
        file_system_.operations().end());
  }

  MemoryFileSystem file_system_;
  MemTable memtable_{BytewiseComparator()};
};

TEST_F(CommitGroupTest, LogsSyncsAndInserts) {
  const auto log = OpenLog();
  WriteBatch group = Batch({{"a", "1"}, {"b", "2"}});
  ASSERT_TRUE(PrepareGroup(group, 7).has_value());

  std::size_t start = file_system_.operations().size();
  ASSERT_TRUE(CommitGroup(group, false, *log, memtable_).has_value());
  EXPECT_EQ(Operations(start), (std::vector<std::string>{"append 000005.log", "append 000005.log",
                                                         "flush 000005.log"}));
  EXPECT_EQ(Lookup(memtable_, "a", 7), "1");
  EXPECT_EQ(Lookup(memtable_, "b", 8), "2");

  WriteBatch synced = Batch({{"c", "3"}});
  ASSERT_TRUE(PrepareGroup(synced, 9).has_value());
  start = file_system_.operations().size();
  ASSERT_TRUE(CommitGroup(synced, true, *log, memtable_).has_value());
  EXPECT_EQ(Operations(start).back(), "sync 000005.log");
  EXPECT_EQ(Lookup(memtable_, "c", 9), "3");

  // The log holds both groups, each with its sequence.
  auto file = file_system_.OpenSequential(std::filesystem::path("db") / "000005.log");
  ASSERT_TRUE(file.has_value());
  WalReader reader(std::move(file).value());
  std::vector<SequenceNumber> sequences;
  while (true) {
    auto event = reader.ReadNext();
    ASSERT_TRUE(event.has_value());
    if (!event->has_value()) {
      break;
    }
    const auto& record = std::get<WalLogicalRecord>(**event);
    sequences.push_back(WriteBatchReader::Open(record.data).value().sequence());
  }
  EXPECT_EQ(sequences, (std::vector<SequenceNumber>{7, 9}));
}

TEST_F(CommitGroupTest, InsertsNothingAfterALogFailure) {
  for (std::size_t failing = 0; failing < 4; ++failing) {
    SCOPED_TRACE(failing);
    MemTable memtable(BytewiseComparator());
    const auto log = OpenLog();
    WriteBatch group = Batch({{"a", "1"}});
    ASSERT_TRUE(PrepareGroup(group, 3).has_value());
    file_system_.FailOperation(file_system_.operations().size() + failing,
                               Error::Io("injected failure"));

    const Status failed = CommitGroup(group, true, *log, memtable);

    ASSERT_FALSE(failed.has_value());
    EXPECT_EQ(failed.error().message(), "injected failure");
    EXPECT_EQ(Lookup(memtable, "a", 3), "<missing>");
  }
}

// Scripts the prepare and commit functions of a queue. Each step can be told
// to wait, with the lock released, until the test opens its gate, and to fail
// or throw. Commits record their groups.
class QueueHarness {
 public:
  QueueHarness()
      : queue_([this](std::unique_lock<std::mutex>& lock) { return Step(lock, prepare_); },
               [this](std::unique_lock<std::mutex>& lock, WriteBatch& group, bool sync) {
                 groups_.push_back(Keys(group) + (sync ? " (sync)" : ""));
                 return Step(lock, commit_);
               }) {}

  struct Script {
    // Steps to run before this one fails, waits, or throws; zero means the next.
    std::optional<int> fail_at;
    std::optional<int> wait_at;
    std::optional<int> throw_at;
    // Whether the step releases the lock before it throws, as an interrupted
    // commit may.
    bool unlock_to_throw = false;
    int calls = 0;
    bool gate_open = false;
    bool waiting = false;
  };

  Script& prepare() { return prepare_; }
  Script& commit() { return commit_; }
  std::mutex& mutex() { return mutex_; }
  WriteQueue& queue() { return queue_; }

  std::vector<std::string> groups() {
    std::lock_guard lock(mutex_);
    return groups_;
  }

  // Starts a thread that writes the batch and records its status.
  void StartWriter(std::string key, bool sync, std::size_t value_size = 1,
                   SequenceNumber sequence = 0) {
    threads_.emplace_back([this, key = std::move(key), sync, value_size, sequence] {
      WriteBatch batch = Batch({{key, std::string(value_size, 'v')}});
      EXPECT_TRUE(batch.SetSequence(sequence).has_value());
      std::string result;
      try {
        std::unique_lock lock(mutex_);
        const Status status = queue_.Write(lock, batch, sync);
        result = status.has_value() ? "ok" : std::string(status.error().message());
      } catch (const std::runtime_error& error) {
        result = std::string("threw ") + error.what();
      }
      std::lock_guard lock(mutex_);
      results_[key] = result;
    });
  }

  // Waits until the queue holds the writers.
  void WaitForQueue(std::size_t writers) {
    while (true) {
      std::unique_lock lock(mutex_);
      if (queue_.size() >= writers) {
        return;
      }
      lock.unlock();
      std::this_thread::yield();
    }
  }

  // Waits until a step waits at its gate.
  void WaitForGate(Script& script) {
    std::unique_lock lock(mutex_);
    changed_.wait(lock, [&] { return script.waiting; });
  }

  void OpenGate(Script& script) {
    std::lock_guard lock(mutex_);
    script.gate_open = true;
    changed_.notify_all();
  }

  std::map<std::string, std::string> Join() {
    for (std::thread& thread : threads_) {
      thread.join();
    }
    threads_.clear();
    std::lock_guard lock(mutex_);
    return results_;
  }

 private:
  Status Step(std::unique_lock<std::mutex>& lock, Script& script) {
    const int call = script.calls++;
    if (script.wait_at == call) {
      script.waiting = true;
      changed_.notify_all();
      changed_.wait(lock, [&] { return script.gate_open; });
      script.waiting = false;
    }
    if (script.throw_at == call) {
      if (script.unlock_to_throw) {
        lock.unlock();
      }
      throw std::runtime_error("step failed");
    }
    if (script.fail_at == call) {
      return std::unexpected(Error::Io("step failed"));
    }
    return {};
  }

  std::mutex mutex_;
  std::condition_variable changed_;
  Script prepare_;
  Script commit_;
  std::vector<std::string> groups_;
  std::map<std::string, std::string> results_;
  std::vector<std::thread> threads_;
  WriteQueue queue_;
};

TEST(WriteQueueTest, CommitsASingleWriterAlone) {
  QueueHarness harness;
  harness.StartWriter("w1", true);
  EXPECT_EQ(harness.Join(), (std::map<std::string, std::string>{{"w1", "ok"}}));
  EXPECT_EQ(harness.groups(), (std::vector<std::string>{"w1 (sync)"}));
  std::unique_lock lock(harness.mutex());
  EXPECT_EQ(harness.queue().size(), 0U);
}

TEST(WriteQueueTest, GroupsWritersThatQueueWhileTheFrontPrepares) {
  QueueHarness harness;
  harness.prepare().wait_at = 0;
  harness.StartWriter("w1", false);
  harness.WaitForGate(harness.prepare());
  harness.StartWriter("w2", false);
  harness.WaitForQueue(2);
  harness.StartWriter("w3", false);
  harness.WaitForQueue(3);

  harness.OpenGate(harness.prepare());

  EXPECT_EQ(harness.Join(),
            (std::map<std::string, std::string>{{"w1", "ok"}, {"w2", "ok"}, {"w3", "ok"}}));
  EXPECT_EQ(harness.groups(), (std::vector<std::string>{"w1,w2,w3"}));
}

TEST(WriteQueueTest, FailsOnlyTheFrontWriterWhenPreparingFails) {
  QueueHarness harness;
  harness.prepare().wait_at = 0;
  harness.prepare().fail_at = 0;
  harness.StartWriter("w1", false);
  harness.WaitForGate(harness.prepare());
  harness.StartWriter("w2", false);
  harness.WaitForQueue(2);
  harness.StartWriter("w3", false);
  harness.WaitForQueue(3);

  harness.OpenGate(harness.prepare());

  EXPECT_EQ(harness.Join(), (std::map<std::string, std::string>{
                                {"w1", "step failed"}, {"w2", "ok"}, {"w3", "ok"}}));
  EXPECT_EQ(harness.groups(), (std::vector<std::string>{"w2,w3"}));
}

TEST(WriteQueueTest, GivesEveryWriterOfAGroupItsStatus) {
  QueueHarness harness;
  harness.prepare().wait_at = 0;
  harness.commit().fail_at = 0;
  harness.StartWriter("w1", false);
  harness.WaitForGate(harness.prepare());
  harness.StartWriter("w2", false);
  harness.WaitForQueue(2);

  harness.OpenGate(harness.prepare());

  EXPECT_EQ(harness.Join(),
            (std::map<std::string, std::string>{{"w1", "step failed"}, {"w2", "step failed"}}));
  EXPECT_EQ(harness.groups(), (std::vector<std::string>{"w1,w2"}));
}

TEST(WriteQueueTest, KeepsSyncWritersOutOfGroupsThatDoNotSync) {
  QueueHarness harness;
  harness.prepare().wait_at = 0;
  harness.StartWriter("w1", false);
  harness.WaitForGate(harness.prepare());
  harness.StartWriter("w2", false);
  harness.WaitForQueue(2);
  harness.StartWriter("w3", true);
  harness.WaitForQueue(3);
  harness.StartWriter("w4", false);
  harness.WaitForQueue(4);
  harness.StartWriter("w5", true);
  harness.WaitForQueue(5);

  harness.OpenGate(harness.prepare());

  harness.Join();
  // A group that syncs takes writers whether or not they ask for a sync.
  EXPECT_EQ(harness.groups(), (std::vector<std::string>{"w1,w2", "w3,w4,w5 (sync)"}));
}

TEST(WriteQueueTest, LimitsTheSizeOfAGroup) {
  constexpr std::size_t KiB = 1024;
  for (const auto& [sizes, expected] :
       std::vector<std::pair<std::vector<std::size_t>, std::vector<std::string>>>{
           // A small front batch lets the group grow by 128 KiB.
           {{10, 100 * KiB, 40 * KiB}, {"w1,w2", "w3"}},
           // A large one lets it grow to 1 MiB.
           {{200 * KiB, 500 * KiB, 400 * KiB}, {"w1,w2", "w3"}},
           {{200 * KiB, 500 * KiB, 300 * KiB}, {"w1,w2,w3"}}}) {
    SCOPED_TRACE(sizes[1]);
    QueueHarness harness;
    harness.prepare().wait_at = 0;
    harness.StartWriter("w1", false, sizes[0]);
    harness.WaitForGate(harness.prepare());
    harness.StartWriter("w2", false, sizes[1]);
    harness.WaitForQueue(2);
    harness.StartWriter("w3", false, sizes[2]);
    harness.WaitForQueue(3);

    harness.OpenGate(harness.prepare());

    harness.Join();
    EXPECT_EQ(harness.groups(), expected);
  }
}

TEST(WriteQueueTest, GroupsBatchesWhateverSequencesTheyHold) {
  QueueHarness harness;
  harness.prepare().wait_at = 0;
  // The front batch's own sequence leaves no room for more entries.
  harness.StartWriter("w1", false, 1, MaxSequenceNumber);
  harness.WaitForGate(harness.prepare());
  harness.StartWriter("w2", false, 1, MaxSequenceNumber);
  harness.WaitForQueue(2);

  harness.OpenGate(harness.prepare());

  EXPECT_EQ(harness.Join(), (std::map<std::string, std::string>{{"w1", "ok"}, {"w2", "ok"}}));
  EXPECT_EQ(harness.groups(), (std::vector<std::string>{"w1,w2"}));
}

TEST(WriteQueueTest, LeavesAWriterThatArrivesDuringACommitForTheNextGroup) {
  QueueHarness harness;
  harness.commit().wait_at = 0;
  harness.commit().fail_at = 0;
  harness.StartWriter("w1", false);
  harness.WaitForGate(harness.commit());
  harness.StartWriter("w2", false);
  harness.WaitForQueue(2);

  harness.OpenGate(harness.commit());

  EXPECT_EQ(harness.Join(),
            (std::map<std::string, std::string>{{"w1", "step failed"}, {"w2", "ok"}}));
  EXPECT_EQ(harness.groups(), (std::vector<std::string>{"w1", "w2"}));
}

TEST(WriteQueueTest, ReleasesTheQueueWhenAStepThrows) {
  {
    // Only the front writer is affected when preparing throws, here with the
    // lock held.
    QueueHarness harness;
    harness.prepare().wait_at = 0;
    harness.prepare().throw_at = 0;
    harness.StartWriter("w1", false);
    harness.WaitForGate(harness.prepare());
    harness.StartWriter("w2", false);
    harness.WaitForQueue(2);
    harness.OpenGate(harness.prepare());
    EXPECT_EQ(harness.Join(),
              (std::map<std::string, std::string>{{"w1", "threw step failed"}, {"w2", "ok"}}));
    EXPECT_EQ(harness.groups(), (std::vector<std::string>{"w2"}));
  }
  {
    // The writers of a group whose commit throws are aborted, and a writer
    // that arrived during the commit leads the next group.
    QueueHarness harness;
    harness.prepare().wait_at = 0;
    harness.commit().wait_at = 0;
    harness.commit().throw_at = 0;
    harness.commit().unlock_to_throw = true;
    harness.StartWriter("w1", false);
    harness.WaitForGate(harness.prepare());
    harness.StartWriter("w2", false);
    harness.WaitForQueue(2);
    harness.OpenGate(harness.prepare());
    harness.WaitForGate(harness.commit());
    harness.StartWriter("w3", false);
    harness.WaitForQueue(3);
    harness.OpenGate(harness.commit());
    const std::map<std::string, std::string> results = harness.Join();
    EXPECT_EQ(results.at("w1"), "threw step failed");
    EXPECT_NE(results.at("w2").find("aborted"), std::string::npos) << results.at("w2");
    EXPECT_EQ(results.at("w3"), "ok");
    EXPECT_EQ(harness.groups(), (std::vector<std::string>{"w1,w2", "w3"}));
  }
}

TEST(WriteQueueTest, CommitsEveryWriteOnceInOrder) {
  std::mutex mutex;
  std::vector<std::string> committed;
  WriteQueue queue([](std::unique_lock<std::mutex>&) -> Status { return {}; },
                   [&](std::unique_lock<std::mutex>& lock, WriteBatch& group, bool) -> Status {
                     const std::string keys = Keys(group);
                     // Let writers queue while the lock is released.
                     lock.unlock();
                     std::this_thread::yield();
                     lock.lock();
                     std::size_t start = 0;
                     while (start <= keys.size()) {
                       const std::size_t end = std::min(keys.find(',', start), keys.size());
                       committed.push_back(keys.substr(start, end - start));
                       start = end + 1;
                     }
                     return {};
                   });
  constexpr int Threads = 8;
  constexpr int Writes = 200;
  std::vector<std::thread> threads;
  for (int thread = 0; thread < Threads; ++thread) {
    threads.emplace_back([&, thread] {
      for (int write = 0; write < Writes; ++write) {
        const std::string key = std::to_string(thread) + ":" + std::to_string(write);
        const WriteBatch batch = Batch({{key, "v"}});
        std::unique_lock lock(mutex);
        EXPECT_TRUE(queue.Write(lock, batch, write % 10 == 0).has_value());
      }
    });
  }
  for (std::thread& thread : threads) {
    thread.join();
  }

  ASSERT_EQ(committed.size(), static_cast<std::size_t>(Threads * Writes));
  std::map<int, int> next;
  for (const std::string& key : committed) {
    const int thread = std::stoi(key.substr(0, key.find(':')));
    const int write = std::stoi(key.substr(key.find(':') + 1));
    EXPECT_EQ(write, next[thread]) << key;
    next[thread] = write + 1;
  }
}

}  // namespace
}  // namespace modern_leveldb
