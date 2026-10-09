# 10. Guided Labs

[Learning path](README.md) | Side reading: [LevelDB comparison](11-leveldb-comparison.md)

The purpose is to predict behavior before running a test, then explain the
result from the implementation. Passing commands alone do not complete a lab.

## Setup and safety

Use the prerequisites in the current
[getting-started guide](../reference/getting-started.md). Database-based labs
use the Linux/macOS POSIX backend or the admitted x64/MSVC Windows backend.
Read [platform support and durability](../reference/platform-support-and-durability.md)
before accepting the weaker Windows namespace guarantee.

Keep two locations distinct:

- `REPO_ROOT`: this repository; every preset and CTest command runs here.
- `LEARNING_SCRATCH`: an external disposable consumer directory used only by
  Lab 2 and the database inspected again in Lab 9.

On Linux/macOS, initialize and build from the repository root:

```bash
export REPO_ROOT="/absolute/path/to/modern-level-db"
cd "$REPO_ROOT"
cmake --preset dev-debug
cmake --build --preset dev-debug
```

On Windows, start an initialized x64 MSVC PowerShell and run:

```powershell
$env:REPO_ROOT = 'C:\absolute\path\to\modern-level-db'
Set-Location $env:REPO_ROOT
cmake --preset windows-debug
cmake --build --preset windows-debug
```

Every focused test block below shows both preset forms. Add `-N` before the
selector to list matching tests without executing them. The Lab 2 consumer
commands use explicit source/build/database paths, so they do not silently
change the working directory required by later repository commands.

Use only disposable directories. Do not truncate, replace, or edit files of
an open database. Use existing fault-injection tests for corruption/crash
experiments rather than damaging a real database.

## Lab 1: decode a batch on paper

Read [lesson 02](02-bytes-and-formats.md).
For its two-operation batch, write down the offset of the count, each tag,
each length, and each key/value.
Predict what happens if the count becomes three or the final key is removed.

Then run:

```console
# Linux/macOS
ctest --preset dev-debug -L unit -R 'CodingTest|InternalKeyTest|WriteBatch'
# Windows
ctest --preset windows-debug -L unit -R 'CodingTest|InternalKeyTest|WriteBatch'
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
#if defined(_WIN32)
  // Required for every owned native Windows open.
  options.allow_weak_namespace_durability = true;
#endif
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
set(MODERN_LEVELDB_BUILD_TOOLS OFF)
add_subdirectory("${MODERN_LEVELDB_SOURCE_DIR}" modern-leveldb)
add_executable(learning_demo learning_demo.cc)
target_link_libraries(learning_demo PRIVATE modern_leveldb::modern_leveldb)
```

Set the scratch location, create the two files above there, and build without
changing the repository command context. On Linux/macOS:

```bash
export LEARNING_SCRATCH="/absolute/path/to/modern-leveldb-learning"
export LEARNING_DB="$LEARNING_SCRATCH/lesson-db-01"
mkdir -p "$LEARNING_SCRATCH"
cmake -S "$LEARNING_SCRATCH" -B "$LEARNING_SCRATCH/build" -G Ninja \
  -DCMAKE_BUILD_TYPE=Debug \
  -DMODERN_LEVELDB_SOURCE_DIR="$REPO_ROOT"
cmake --build "$LEARNING_SCRATCH/build"
"$LEARNING_SCRATCH/build/learning_demo" "$LEARNING_DB"
```

On Windows:

```powershell
$env:LEARNING_SCRATCH = 'C:\absolute\path\to\modern-leveldb-learning'
$env:LEARNING_DB = Join-Path $env:LEARNING_SCRATCH 'lesson-db-01'
New-Item -ItemType Directory -Force $env:LEARNING_SCRATCH | Out-Null
cmake -S $env:LEARNING_SCRATCH -B "$env:LEARNING_SCRATCH\build" -G Ninja `
  -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_COMPILER=cl -DCMAKE_CXX_COMPILER=cl `
  "-DMODERN_LEVELDB_SOURCE_DIR=$env:REPO_ROOT"
cmake --build "$env:LEARNING_SCRATCH\build"
& "$env:LEARNING_SCRATCH\build\learning_demo.exe" $env:LEARNING_DB
```

Use Visual Studio's bundled CMake if another distribution shadows it in
`PATH`; the supported Windows boundary requires native MSVC, not MinGW.

Expected output:

```text
snapshot=red; latest=absent
```

For another run, choose a new database path. Keep `LEARNING_DB` pointing to
the closed first database if you plan to inspect it in Lab 9.
The demo checks logical visibility; its default async writes do not
establish a power-loss durability guarantee. The Windows opt-in admits the
platform's documented weak namespace boundary; it does not strengthen it.

Return explicitly to the repository root and study the related public
contracts:

```bash
cd "$REPO_ROOT"
ctest --preset dev-debug -L unit -R '^PublicDatabaseTest\.'
```

```powershell
Set-Location $env:REPO_ROOT
ctest --preset windows-debug -L unit -R '^PublicDatabaseTest\.'
```

**Completion:** explain why destroying the Snapshot handle cannot invalidate
an iterator that retained its registration, and why a child can keep the
engine open after the Database handle disappears.

## Lab 3: find the WAL crash boundary

Before running anything, sketch a batch fragmented across two WAL blocks.
Predict the result when its Last fragment is missing.
Predict the different result of a complete fragment with a bad checksum.

```console
# Linux/macOS
ctest --preset dev-debug -L unit -R 'WalReaderTest.*Truncated|WalReaderTest.*Checksum|RecoveryTest.*SkipsDamaged'
# Windows
ctest --preset windows-debug -L unit -R 'WalReaderTest.*Truncated|WalReaderTest.*Checksum|RecoveryTest.*SkipsDamaged'
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

```console
# Linux/macOS
ctest --preset dev-debug -L unit -R 'WriteQueueTest|CommitGroupTest'
# Windows
ctest --preset windows-debug -L unit -R 'WriteQueueTest|CommitGroupTest'
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

```console
# Linux/macOS
ctest --preset dev-debug -L unit -R '^CompactionTest\.DropsOnlyEntriesThatNoSnapshotReads$'
# Windows
ctest --preset windows-debug -L unit -R '^CompactionTest\.DropsOnlyEntriesThatNoSnapshotReads$'
```

Read that test and `BaseLevel::IsBaseLevelForKey` in
[`compaction.cc`](../../src/engine/compaction.cc).

**Completion:** explain both the oldest-snapshot rule and the
lower-level-overlap rule. "Deleted means garbage" is not a sufficient answer.

## Lab 6: ownership is not cache membership

Predict whether an old cache handle stays valid after its key is replaced.
Predict whether pinned entries can exceed configured capacity.

```console
# Linux/macOS
ctest --preset dev-debug -L unit -R 'ShardedLruCacheTest\.(ReplacementKeepsOldHandleAlive|PinnedEntriesMayExceedCapacity)'
# Windows
ctest --preset windows-debug -L unit -R 'ShardedLruCacheTest\.(ReplacementKeepsOldHandleAlive|PinnedEntriesMayExceedCapacity)'
```

Read [`sharded_lru_cache_test.cc`](../../tests/unit/cache/sharded_lru_cache_test.cc).
Draw two lifetimes: membership in the cache and ownership through a handle.

**Completion:** explain why an eviction budget is not a process-wide hard
memory limit, and identify the owner of an uncompressed mapped block.

## Lab 7: observe LSM and maintenance state

Read [lesson 03](03-memory-and-mvcc.md),
[lesson 06](06-reads-and-iterators.md), and
[lesson 07](07-writes-and-compaction.md), then run:

```console
# Linux/macOS
ctest --preset dev-debug -L unit -R 'DatabaseTest\.(ReportsPublishedStateAndExplicitSnapshots|ReportsImmutableAndProtectedFlushState)|PublicDatabaseTest\.(ReportsOwningDatabaseStateAndRetainedSnapshotRegistration|DatabaseStateOutlivesItsDatabase)'
# Windows
ctest --preset windows-debug -L unit -R 'DatabaseTest\.(ReportsPublishedStateAndExplicitSnapshots|ReportsImmutableAndProtectedFlushState)|PublicDatabaseTest\.(ReportsOwningDatabaseStateAndRetainedSnapshotRegistration|DatabaseStateOutlivesItsDatabase)'
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
Build the platform profiling preset from `REPO_ROOT`.
On Linux/macOS:

```bash
cmake --preset profiling
cmake --build --preset profiling
```

On Windows:

```powershell
cmake --preset windows-profiling
cmake --build --preset windows-profiling
```

Collect a read diagnostic report, not a throughput score:
Each run requires a new output directory.

```bash
python3 tools/run_performance.py \
  --binary build/profiling/benchmarks/modern_leveldb_read_diagnostics \
  --case modern/readrandom/65536 \
  --read-diagnostics \
  --output build/learning/readrandom-diagnostics-01
```

```powershell
python tools\run_performance.py `
  --binary build\windows-profiling\benchmarks\modern_leveldb_read_diagnostics.exe `
  --case modern/readrandom/65536 `
  --read-diagnostics `
  --output build\learning\windows-readrandom-diagnostics-01
```

Find table/block cache counters, mapped/copied block counters, decoded
entries, and sampled stage timings.
For a second comparison, use a distinct output directory and add the copied
control: `--modern-file-access pread` on POSIX or
`--modern-file-access copied` on Windows.
Do not combine diagnostic mode with smoke, repetition, or CPU-capture flags.

**Completion:** explain why block-cache misses alone do not count physical
device reads, why compressed mapped blocks still need owned output, and why
inclusive sampled stage durations should not be summed.
Use the current
[benchmarking and profiling guide](../development/benchmarking-and-profiling.md)
before interpreting speed.

## Lab 9: inspect persistent files without treating text as a backup

Build the normal Debug preset, which produces the native tool.
Run from `REPO_ROOT`; if this is a new shell, restore the variables from the
setup section and return to that directory first.
On Linux/macOS:

```bash
cmake --build --preset dev-debug
./build/dev-debug/tools/modern_leveldb_tool --help
```

On Windows:

```powershell
cmake --build --preset windows-debug
& .\build\windows-debug\tools\modern_leveldb_tool.exe --help
```

Run its focused format tests:

```console
# Linux/macOS
ctest --preset dev-debug -L unit -R 'DumpFileTest|DumpCommandTest|PosixOutputTest'
# Windows
ctest --preset windows-debug -L unit -R 'DumpFileTest|DumpCommandTest|WindowsOutputTest|NativeDumpCommandTest'
```

Then choose one canonical file from the **closed disposable database** stored
in `LEARNING_DB` by Lab 2.

On Linux/macOS:

```bash
ls "$LEARNING_DB"
MANIFEST_PATH=$(printf '%s\n' "$LEARNING_DB"/MANIFEST-* | head -n 1)
test -f "$MANIFEST_PATH"
./build/dev-debug/tools/modern_leveldb_tool dump "$MANIFEST_PATH"
```

On Windows:

```powershell
Get-ChildItem $env:LEARNING_DB
$manifest = Get-ChildItem $env:LEARNING_DB -Filter 'MANIFEST-*' |
  Select-Object -First 1
if ($null -eq $manifest) { throw 'No MANIFEST file found' }
& .\build\windows-debug\tools\modern_leveldb_tool.exe dump $manifest.FullName
```

The commands discover the actual MANIFEST name because file numbers depend
on the database's history. Repeat with a log or table file from the same
closed directory. The output connects:

```text
WAL record       -> write batch operations and assigned sequences
MANIFEST record  -> file/counter VersionEdit fields
SSTable entry    -> user key, sequence, value/deletion kind, and value
```

Corruption that the selected traversal encounters produces useful partial
stdout, an escaped error on stderr, and a nonzero exit status. A successful
table dump is not a whole-file verifier: unrequested filter metadata and
unreferenced bytes may not be read.

Never redirect the output onto an input or another database file. Dump text
can expose application data and is deliberately not accepted as restore
input. For an offline backup, close the database and copy its original binary
directory instead.

**Completion:** explain why forward SSTable dumping does not need the original
custom comparator, why a truncated final WAL fragment is benign EOF, and why
diagnostic output is not a checkpoint or repair operation.

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
