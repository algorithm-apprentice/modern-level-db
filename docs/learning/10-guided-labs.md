# 10. Guided Labs

[Learning path](README.md) | Side reading: [LevelDB comparison](11-leveldb-comparison.md)

The purpose is to predict behavior before running a test, then explain the
result from the implementation. Passing commands alone do not complete a lab.

## Setup and safety

Use a C++23-capable toolchain, CMake, Ninja, and the prerequisites in the
root [README](../../README.md).
Database-based labs require the current Linux/macOS POSIX backend; portable
format and memory components can also be studied on other build platforms.
Run these once from the repository root:

```bash
cmake --preset dev-debug
cmake --build --preset dev-debug
```

All CTest commands below run from that root.
For focused selection, `ctest --preset dev-debug -N -R 'pattern'` lists
matching tests without executing them.
The consumer demo is the one section whose commands run in a separate
scratch directory.

Use only disposable directories. Do not truncate, replace, or edit files of
an open database. Use existing fault-injection tests for corruption/crash
experiments rather than damaging a real database.

## Lab 1: decode a batch on paper

Read [lesson 02](02-bytes-and-formats.md).
For its two-operation batch, write down the offset of the count, each tag,
each length, and each key/value.
Predict what happens if the count becomes three or the final key is removed.

Then run:

```bash
ctest --preset dev-debug -L unit -R 'CodingTest|InternalKeyTest|WriteBatch'
```

Open [`write_batch_test.cc`](../../tests/unit/format/write_batch_test.cc)
and find a test for the malformed case you predicted.
Follow `WriteBatchReader::Open`, not the trusted reader.

**Completion:** you can distinguish a valid WAL frame containing an invalid
batch from a valid batch with an empty value.

## Lab 2: run a snapshot example

Create `learning_demo.cc` and `CMakeLists.txt` in a new scratch consumer
directory, separate from the repository and any database directories.
The example deliberately refuses to reopen an existing database so an
accidental repeated run does not overwrite earlier data.

### learning_demo.cc

```cpp
#include <expected>
#include <filesystem>
#include <iostream>

#include "modern_leveldb/db.h"

namespace ml = modern_leveldb;

ml::Status RunDemo(ml::Database& database) {
  ml::Status written = database.Put(ml::AsBytes("color"), ml::AsBytes("red"));
  if (!written.has_value()) {
    return written;
  }

  auto snapshot = database.GetSnapshot();
  if (!snapshot.has_value()) {
    return std::unexpected(snapshot.error());
  }

  written = database.Put(ml::AsBytes("color"), ml::AsBytes("blue"));
  if (!written.has_value()) {
    return written;
  }

  ml::ReadOptions old_view{.snapshot = &*snapshot};
  auto old = database.Get(ml::AsBytes("color"), old_view);
  if (!old.has_value()) {
    return std::unexpected(old.error());
  }
  if (!old->has_value()) {
    return std::unexpected(ml::Error::Corruption("snapshot value is missing"));
  }
  if (ml::AsStringView(**old) != "red") {
    return std::unexpected(ml::Error::Corruption("unexpected snapshot value"));
  }

  ml::Status deleted = database.Delete(ml::AsBytes("color"));
  if (!deleted.has_value()) {
    return deleted;
  }
  auto latest = database.Get(ml::AsBytes("color"));
  if (!latest.has_value()) {
    return std::unexpected(latest.error());
  }
  if (latest->has_value()) {
    return std::unexpected(ml::Error::Corruption("deleted key is still visible"));
  }
  auto still_old = database.Get(ml::AsBytes("color"), old_view);
  if (!still_old.has_value()) {
    return std::unexpected(still_old.error());
  }
  if (!still_old->has_value()) {
    return std::unexpected(ml::Error::Corruption("snapshot lost its old value"));
  }
  if (ml::AsStringView(**still_old) != "red") {
    return std::unexpected(ml::Error::Corruption("snapshot changed after deletion"));
  }
  return {};
}

int main(int argc, char* argv[]) {
  if (argc != 2) {
    std::cerr << "usage: learning_demo NEW_DATABASE_DIRECTORY\n";
    return 2;
  }
  ml::Options options;
  options.create_if_missing = true;
  options.error_if_exists = true;
  auto opened = ml::Database::Open(options, std::filesystem::path(argv[1]));
  if (!opened.has_value()) {
    std::cerr << opened.error().ToString() << '\n';
    return 1;
  }
  const ml::Status result = RunDemo(*opened);
  if (!result.has_value()) {
    std::cerr << result.error().ToString() << '\n';
    return 1;
  }
  std::cout << "snapshot=red; latest=absent\n";
  return 0;
}
```

### CMakeLists.txt

```cmake
cmake_minimum_required(VERSION 3.25)
project(modern_leveldb_learning LANGUAGES C CXX)

if(NOT DEFINED MODERN_LEVELDB_SOURCE_DIR)
  message(FATAL_ERROR "Set MODERN_LEVELDB_SOURCE_DIR to the repository path")
endif()

set(MODERN_LEVELDB_BUILD_TESTS OFF)
add_subdirectory("${MODERN_LEVELDB_SOURCE_DIR}" modern-leveldb)
add_executable(learning_demo learning_demo.cc)
target_link_libraries(learning_demo PRIVATE modern_leveldb::modern_leveldb)
```

Run from the scratch consumer directory, replacing the repository path:

```bash
cmake -S . -B build -G Ninja \
  -DMODERN_LEVELDB_SOURCE_DIR=/absolute/path/to/modern-leveldb
cmake --build build
./build/learning_demo ./lesson-db-01
```

Expected output:

```text
snapshot=red; latest=absent
```

For another run, choose a new database path.
The demo checks logical visibility; its default async writes do not
establish a power-loss durability guarantee.

Back in the repository, study the related public contracts:

```bash
ctest --preset dev-debug -L unit -R '^PublicDatabaseTest\.'
```

**Completion:** explain why destroying the Snapshot handle cannot invalidate
an iterator that retained its registration, and why a child can keep the
engine open after the Database handle disappears.

## Lab 3: find the WAL crash boundary

Before running anything, sketch a batch fragmented across two WAL blocks.
Predict the result when its Last fragment is missing.
Predict the different result of a complete fragment with a bad checksum.

```bash
ctest --preset dev-debug -L unit \
  -R 'WalReaderTest.*Truncated|WalReaderTest.*Checksum|RecoveryTest.*SkipsDamaged'
```

Read the relevant tests in
[`wal_io_test.cc`](../../tests/unit/wal/wal_io_test.cc) and
[`recovery_test.cc`](../../tests/unit/engine/recovery_test.cc).
Then compare EOF, corruption events, and I/O failures in `WalReader`.

**Completion:** explain why no valid logical batch is produced from an
incomplete fragment sequence, and why that is not permission to ignore all
metadata corruption.

## Lab 4: predict group commit

Draw this queued sequence:

```text
A async -> B async -> C sync -> D async
```

Assume all are queued before A builds its group and all fit size limits.
Predict the first group.
Then draw the same sequence with A changed to sync.

```bash
ctest --preset dev-debug -L unit -R 'WriteQueueTest|CommitGroupTest'
```

Read `BuildGroup` and
`KeepsSyncWritersOutOfGroupsThatDoNotSync` in
[`write_path_test.cc`](../../tests/unit/engine/write_path_test.cc).

**Completion:** A async groups with B, then stops before C.
With A sync, all four can join subject to size limits.
Explain why batching is not a background timer-based delay.

## Lab 5: decide what compaction may drop

Use the versions `105, 101, 99, 70` with oldest snapshot 100 from
[lesson 07](07-writes-and-compaction.md).
Mark every kept/dropped entry before examining the code.
Then consider a tombstone at 101 with an older value in a lower level.

```bash
ctest --preset dev-debug -L unit \
  -R '^CompactionTest\.DropsOnlyEntriesThatNoSnapshotReads$'
```

Read that test and `BaseLevel::IsBaseLevelForKey` in
[`compaction.cc`](../../src/engine/compaction.cc).

**Completion:** explain both the oldest-snapshot rule and the
lower-level-overlap rule. "Deleted means garbage" is not a sufficient answer.

## Lab 6: ownership is not cache membership

Predict whether an old cache handle stays valid after its key is replaced.
Predict whether pinned entries can exceed configured capacity.

```bash
ctest --preset dev-debug -L unit \
  -R 'ShardedLruCacheTest\.(ReplacementKeepsOldHandleAlive|PinnedEntriesMayExceedCapacity)'
```

Read [`sharded_lru_cache_test.cc`](../../tests/unit/cache/sharded_lru_cache_test.cc).
Draw two lifetimes: membership in the cache and ownership through a handle.

**Completion:** explain why an eviction budget is not a process-wide hard
memory limit, and identify the owner of an uncompressed mmap block.

## Lab 7: observe LSM and maintenance state

Read [lessons 03, 06, and 07](README.md), then run:

```bash
ctest --preset dev-debug -L unit \
  -R 'DatabaseTest\.(ReportsPublishedStateAndExplicitSnapshots|ReportsImmutableAndProtectedFlushState)|PublicDatabaseTest\.(ReportsOwningDatabaseStateAndRetainedSnapshotRegistration|DatabaseStateOutlivesItsDatabase)'
```

Follow `Database::GetState` into `DatabaseEngine::GetState`.
For each returned field, identify the mutex-protected source that it copies.
Then follow a write through `CommitWrite` and explain why
`mutable_memtable_bytes` uses a published value instead of reading the live
arena while the database mutex is held.

Predict these transitions before reading the assertions:

```text
new database:
  seven empty levels, sequence 0, mutable arena reservation

explicit snapshot:
  snapshot_count increases and oldest_snapshot_sequence appears

forced rotation:
  immutable_memtable_bytes appears and background_work_scheduled is true

flush output construction:
  protected_output_count becomes one, then clears after installation
```

`file_bytes` is current SSTable metadata, not live user bytes or total
directory usage. Memtable values are arena-reserved bytes, not exact payload
or process RSS. A nonzero protected count is a cleanup invariant and can
remain after an exceptional background termination.

**Completion:** explain why a state value remains valid after the database
changes or closes, why it performs no I/O, and why it is not a metrics stream
or background-progress percentage.

## Lab 8: inspect read costs, not just elapsed time

This is optional and more expensive than the focused unit labs.
Build the profiling preset:

```bash
cmake --preset profiling
cmake --build --preset profiling
```

Collect a read diagnostic report, not a throughput score:

```bash
python3 tools/run_performance.py \
  --binary build/profiling/benchmarks/modern_leveldb_read_diagnostics \
  --case modern/readrandom/65536 \
  --read-diagnostics \
  --output build/learning/readrandom-diagnostics-01
```

Find table/block cache counters, mapped/copied block counters, decoded
entries, and sampled stage timings.
For a second comparison, use a distinct output directory and add
`--modern-file-access pread`.
Do not combine diagnostic mode with smoke, repetition, or CPU-capture flags.

**Completion:** explain why block-cache misses alone do not count physical
device reads, why compressed mmap blocks still need owned output, and why
inclusive sampled stage durations should not be summed.
Use [the profiling guide](../profiling-design.md) before interpreting speed.

## Capstone: explain a complete lifecycle

Without source open, draw:

```text
Open -> sync batch -> Snapshot -> overwrite/delete
     -> rotation -> flush -> compaction
     -> release children -> close -> reopen
```

At every transition, label:

- The visible sequence and the source of the returned value.
- The owner keeping each borrowed object alive.
- The bytes/names that must be durable before acknowledgement or installation.
- The old entries/files that are still protected.
- The error that would prevent further mutation.

Then check the drawing against the source tours, not against class names
from memory.
You have completed the core learning path when you can explain both the
normal lifecycle and one failure boundary in each persistent transition.
