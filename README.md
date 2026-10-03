# Modern LevelDB

Modern LevelDB is a ground-up C++23 reimplementation of the core LevelDB
storage model. The project aims to preserve LevelDB's small, ordered,
single-process key-value semantics while rebuilding the implementation around
explicit ownership, testable durability boundaries, and a strict acyclic
architecture.

The project is currently pre-alpha and is not suitable for production data.

The implemented foundation covers byte views, typed results, comparison,
binary coding, CRC32C, LevelDB-compatible seeded hashing, and a monotonic
arena. The platform foundation also provides a steady clock and a serial,
stop-aware background executor, portable file interfaces, and the first
Linux/macOS POSIX filesystem backend. The format layer now includes the
LevelDB-compatible internal-key trailer, WAL physical-record framing, and
Put/Delete write-batch payload with a validated zero-copy reader. The cache
layer provides a typed sharded LRU with RAII pin handles. Database memory
structures now include an arena-backed, single-writer concurrent-reader skip
list and a packed MemTable with snapshot lookup and ordered iteration. An owned
WAL stream layer provides flushed appends, explicit sync, logical-record
reassembly, and typed corruption events. The metadata layer names and
classifies LevelDB-compatible database files, validates `CURRENT` contents,
encodes and strictly decodes LevelDB-compatible MANIFEST version edits, builds
validated immutable versions from them, and creates, recovers, and appends to
the MANIFEST with durable `CURRENT` installation.
The table layer builds and reads LevelDB-compatible sorted blocks, block
handles, footers, checksummed block trailers, Bloom filters, and filter
blocks, compresses blocks with Snappy or Zstd when they beat LevelDB's space
threshold, writes complete SSTables durably, and reads them through lookups,
bidirectional iteration, and a block cache. The engine layer keeps recently
used tables open in an LRU cache by file number, writes memtables to durable,
verified level-0 tables, and opens databases: it locks the directory, creates
or recovers the version set, replays the logs into level-0 tables, and starts
a new log. Point reads look up a key at a snapshot in the memtables and the
current version, and iterators merge the memtables and the version's tables
and yield the user keys that a snapshot sees in both directions. Writers
queue under the database mutex, and the front writer commits the queued
batches as one group: it checks the group before any I/O, appends it to the
log, syncs the log if asked, and inserts it into the memtable. A flush writes
an immutable memtable to a table and returns the version edit that installs
it at the level LevelDB would choose. Compaction picking scores a version's
levels and chooses a compaction's inputs, grandparents, and trivial moves as
LevelDB does, and running a compaction writes the entries that some snapshot
can still read to new tables of the next level, byte for byte as LevelDB
would. Point reads and sampled iterator reads charge files' seek budgets,
which name a file to compact once they run out, as LevelDB's do. An internal
database engine ties these together: it commits writes, switches full
memtables to a new synced log, flushes immutable memtables in the background,
serves reads and iterators at snapshots, runs size and seek compactions,
slows or stops writes while level 0 backs up, and removes obsolete files. The
public RAII facade exposes database handles, atomic write batches, snapshots,
bidirectional iterators, and an owning typed snapshot of LSM topology and
maintenance state while keeping engine and child-handle lifetimes safe. The
canonical MVP implementation also includes reproducible model,
upstream compatibility, power-loss, sanitizer, fuzz, and benchmark gates.

## Goals

- Provide an embeddable ordered key-value store with `Put`, `Get`, `Delete`,
  atomic write batches, snapshots, and ordered iteration.
- Use modern C++ ownership and error handling without exceptions in normal
  storage-engine control flow.
- Preserve the original LevelDB WAL, MANIFEST, and SSTable formats during the
  first implementation phase.
- Make crash consistency, fault injection, fuzzing, and differential testing
  first-class engineering constraints.
- Keep the implementation substantially smaller and more approachable than a
  feature-complete RocksDB-style engine.
- Design abstractions only for concrete Modern LevelDB call sites; do not add
  general-purpose capabilities or complexity without a current requirement.

## Non-goals

- Distributed storage, replication, or a client-server protocol.
- SQL, secondary indexes, or a relational data model.
- Transactions beyond atomic write batches in the initial implementation.
- Column families, merge operators, remote storage, or a general plugin
  framework.
- Source or binary compatibility with the original LevelDB C++ API.

## Architecture

- [Guided learning path: concepts, code tours, and hands-on labs](docs/learning/README.md)
- [LevelDB architecture analysis](docs/architecture.md)
- [Implementation dependency DAG](docs/dependency-dag.md)
- [Architecture decision records](docs/adr/)
- [Benchmark and profiling design and usage](docs/profiling-design.md)

## Using the library

Link `modern_leveldb::modern_leveldb` and include the public facade:

```cpp
#include "modern_leveldb/db.h"

modern_leveldb::Options options;
options.create_if_missing = true;
// Snappy is the default. Compression::None and Compression::Zstd are also
// available; Zstd levels -5 through 22 are accepted.
// POSIX mmap table reads are the default. Mapped storage faults can terminate
// with SIGBUS instead of returning a typed I/O error. Set false to force
// copied reads and typed read errors.
// options.allow_mmap_reads = false;
// New WAL files are durably created by default. Set false only to match
// LevelDB's weaker WAL-creation durability behavior.
// options.sync_wal_creation = false;
auto opened = modern_leveldb::Database::Open(options, "example-db");
if (!opened.has_value()) {
  return opened.error();
}

modern_leveldb::Database database = std::move(*opened);
auto written =
    database.Put(modern_leveldb::AsBytes("key"), modern_leveldb::AsBytes("value"));
if (!written.has_value()) {
  return written.error();
}

auto value = database.Get(modern_leveldb::AsBytes("key"));
```

For repeated point reads, reuse one caller-owned buffer:

```cpp
std::vector<std::byte> value_buffer;
auto found = database.Get(modern_leveldb::AsBytes("key"), value_buffer);
if (!found.has_value()) {
  return found.error();
}
if (*found) {
  // value_buffer contains the value. Missing/deleted keys leave it unchanged.
}
```

Inspect the current LSM and maintenance state without file I/O or a
background-work barrier:

```cpp
auto state = database.GetState();
if (!state.has_value()) {
  return state.error();
}

const auto& level_zero = state->levels[0];
const std::size_t level_zero_files = level_zero.file_count;
const std::size_t mutable_memtable_bytes = state->mutable_memtable_bytes;
```

The returned value owns its fields. File bytes are recorded SSTable sizes;
memtable bytes are arena reservations rather than exact payload or total
process memory. See
[ADR-0061](docs/adr/0061-typed-database-state-inspection.md) for every field's
snapshot, error, and concurrency contract.

`Database`, `Snapshot`, and `Iterator` are move-only handles. Snapshots and
iterators retain the underlying engine, so their storage remains valid even
if the original `Database` handle is destroyed first. See
[ADR-0038](docs/adr/0038-public-raii-api.md) for the complete contracts.

Custom comparators must provide deterministic, unchanged ordering/equality,
support concurrent calls, preserve separator/successor bounds, and keep the
same semantic identity on every reopen. Changing a comparator's name does
not migrate already sorted data. Optional `bloom_bits_per_key` filtering
requires comparator equality to imply byte equality; leave it unset for
comparators that consider different byte strings equal, such as
case-insensitive comparators, or equivalent keys can be reported missing.

One database process exclusively owns an open database directory. External
modification, replacement, or truncation of live database files is
unsupported. Default POSIX mmap reads may also deliver an in-contract storage
fault as `SIGBUS`; set `allow_mmap_reads = false` when typed `Io` read errors
are required.

## Inspecting storage files

Top-level Linux/macOS builds also produce a read-only diagnostic tool:

```bash
./build/dev-debug/tools/modern_leveldb_tool --help
./build/dev-debug/tools/modern_leveldb_tool dump \
  example-db/MANIFEST-000001 \
  example-db/000002.log \
  example-db/000003.ldb
```

It decodes canonical WAL, MANIFEST, and SSTable files through the same
production readers, escapes binary keys and values, and returns nonzero when
the selected traversal encounters corruption. Its versioned text is for
diagnosis and learning, not backup, restore, repair, or whole-file
certification.

Use it only on files from a closed database, a stable fixture, or a consistent
offline copy. Do not redirect stdout onto an input or any database file.
Output can contain sensitive application keys and values. Set
`MODERN_LEVELDB_BUILD_TOOLS=OFF` to omit the executable.

## Development

The project uses C++23, CMake, Ninja, CTest, and GoogleTest.
Compression uses private Snappy 1.3.1 and Zstd 1.5.7 dependencies; CRC32C uses
Google's runtime-dispatched implementation at the pinned revision recorded in
[ADR-0044](docs/adr/0044-profile-guided-crc32c-acceleration.md). CMake reuses
parent-provided codec targets or fetches pinned source revisions; the fallback
also requires a C compiler. No codec headers enter the public API.

```bash
cmake --preset dev-debug
cmake --build --preset dev-debug
ctest --preset dev-debug -L unit
```

Tests are enabled by default for a standalone build and disabled when the
library is added as a subproject. Either `MODERN_LEVELDB_BUILD_TESTS=OFF` or
`BUILD_TESTING=OFF` disables test targets and their dependencies. When tests
are explicitly enabled by a parent project, its GoogleTest options are
preserved.

Run `ctest --preset dev-debug` to include the `cmake` consumer-configuration
checks as well as the fast `unit` suite.

Every test preset fails when its selection contains no tests. A disabled
tier, stale configuration, or mistyped selector must not count as passing
validation.

The optional extended harness keeps slower verification out of the unit loop:

```bash
cmake --preset compatibility
cmake --build --preset compatibility --target modern_leveldb_extended_tests
ctest --preset compatibility
```

It compares seeded binary-key/snapshot traces with an independent map and a
pinned Google LevelDB reference, opens a frozen upstream database image, and
recovers simulated power-loss images at every mutating I/O boundary. These
database tests require the POSIX backend. The reference source is fetched
only when `MODERN_LEVELDB_BUILD_EXTENDED_TESTS=ON`.

Coverage-guided fuzzing requires a full LLVM toolchain with libFuzzer:

```bash
cmake --preset fuzz
cmake --build --preset fuzz
ctest --preset fuzz
```

On macOS, Homebrew LLVM is keg-only: installing it does not replace
`/usr/bin/clang` or guarantee its tools are on PATH. If needed, install it
with `brew install llvm`, then explicitly select both compilers:

```bash
cmake --preset fuzz --fresh \
  -DCMAKE_C_COMPILER="$(brew --prefix llvm)/bin/clang" \
  -DCMAKE_CXX_COMPILER="$(brew --prefix llvm)/bin/clang++"
cmake --build --preset fuzz
ctest --preset fuzz
```

Use `--fresh` when switching compilers so CMake cannot reset the cache and
silently lose preset options. Keep the same explicit compiler arguments when
reconfiguring that build.

The format and stateful-engine targets use deterministic seed corpora and
explicit input, time, and memory bounds. Reproducers are retained under
`build/fuzz/fuzz/`; replay one by passing its path directly to the relevant
fuzzer executable. See [ADR-0040](docs/adr/0040-compatibility-and-crash-harness.md)
for the persistence model and its limits.

The hardening presets are separate from the fast development loop:

```bash
cmake --preset asan
cmake --build --preset asan
ctest --preset asan

cmake --preset tsan
cmake --build --preset tsan
ctest --preset tsan

cmake --preset benchmarks
cmake --build --preset benchmarks
ctest --preset benchmarks
```

ASan/UBSan runs the full correctness tiers and stops on sanitizer findings.
TSan runs unit and model tests; its CI runtime is macOS Clang. Crash checks
include torn WAL tails and a real child process exiting without database
destructors. The Release benchmark verifies all returned data, records three
same-machine trials against Google LevelDB, and rejects phase medians more
than 20 times the reference. Its raw report is
`build/benchmarks/benchmarks/results.json`; this is a severe-regression guard,
not a throughput SLA.

The `extended-hardening` workflow adds weekly and manually dispatchable
100,000-input fuzz campaigns with sanitizer-instrumented codecs. Failure
inputs and corpora are retained as CI artifacts. See
[ADR-0041](docs/adr/0041-engine-hardening-gates.md) for the fixed budgets,
benchmark methodology, and limitations.

For separate, reproducible read and fixed-work write/mixed measurements, use
the optional Google Benchmark harness:

```bash
cmake --preset profiling
cmake --build --preset profiling
ctest --preset profiling
python3 tools/run_performance.py \
  --binary build/profiling/benchmarks/modern_leveldb_performance \
  --case modern/readrandom/65536 \
  --output build/performance/readrandom-64k
```

Sixteen read cases cover both implementations, two data sizes, random reads,
interior missing-key reads, scans, and reused-iterator seeks. Eight additional
cases cover overwrite, 32-key batches, synchronous writes, and a serialized
50/50 read/write mix:

```bash
python3 tools/run_performance.py \
  --binary build/profiling/benchmarks/modern_leveldb_performance \
  --case modern/overwrite/65536 \
  --output build/performance/overwrite-64k
```

On POSIX profiling builds, add `--reference-file-access pread` to a LevelDB
`readrandom` or `readmissing` case to disable its default read-only mmap path
in that fresh process. This is an attribution control, not a replacement
implementation.

The profiling build also produces a separate diagnostic executable. Its
fixed foreground epoch records counters for source decisions, candidate
files, cache outcomes, reads, block decoding, key comparisons, and copied
bytes, plus sparsely sampled stage durations:

```bash
python3 tools/run_performance.py \
  --binary build/profiling/benchmarks/modern_leveldb_read_diagnostics \
  --case modern/readrandom/65536 \
  --read-diagnostics \
  --output build/performance/readrandom-64k-diagnostics
```

Diagnostic timings are attribution evidence only. Throughput conclusions must
use the uninstrumented `modern_leveldb_performance` executable.

Use `--modern-file-access pread` on a Modern read-family case to force copied
reads instead of the default mmap mode:

```bash
python3 tools/run_performance.py \
  --binary build/profiling/benchmarks/modern_leveldb_performance \
  --case modern/readrandom/65536 \
  --modern-file-access pread \
  --output build/performance/modern-pread-readrandom-64k
```

Modern point-read cases reuse caller-owned output by default. Add
`--modern-result-ownership owning` to a Modern `readrandom` or `readmissing`
case to measure the convenience overload separately.

Mutable cases use fixed operation counts and one repetition, with a fresh
database per process. Use new output paths and independent invocations for
additional samples; `--min-time` and repetitions other than one are rejected.
Their timing covers acknowledged operations and overlapping background CPU,
not drained compaction debt, steady state, or per-request tail latency. See
[the write profiling guide](docs/write-profiling-design.md) for exact budgets,
versioned data, recording-adapter checks, and units.

On macOS with
Apple Clang and Xcode, add `--capture-cpu` and a new output path for a symbolized
Time Profiler capture limited to the measured interval. Native traces stay
local, and profiled timings are not speedup evidence. See
[the profiling guide](docs/profiling-design.md) for exact workloads, lifecycle,
provenance, cleanup, and result semantics. The existing 20x regression gate is
not replaced or relaxed by this harness.

CI also measures unit-test coverage of `src/` and `include/`. Every added or
modified production line and branch must be executed by tests unless an
explicitly justified `GCOVR_EXCL_*` marker excludes it. See
[ADR-0019](docs/adr/0019-test-coverage-policy.md) for the policy and the local
coverage commands.

When every changed path is `README.md` or a Markdown file under `docs/`, CI
runs only lightweight change-routing checks and check acknowledgements,
skipping engine builds, tests, sanitizers, fuzzing, and benchmarks.
Code, test, dependency, CMake, workflow, and mixed changes retain full CI;
uncertain comparisons conservatively run it too. Pull requests consider their
entire diff, not only the latest commit. Existing PR check names remain stable;
push checks use a `push / ` prefix so their results cannot replace PR checks.
On documentation-only runs, platform-labelled checks are acknowledgements on
Ubuntu, not claims that platform builds or tests were executed.
Scheduled/manual hardening campaigns are unchanged. See
[ADR-0048](docs/adr/0048-documentation-only-ci.md) for event and failure semantics.
Run the routing contracts locally with `python3 tests/tools/ci_changes_test.py`.

New or changed production behavior follows test-driven development: start with
a focused failing unit test, implement the smallest correct change, and
refactor while tests remain green. Configuration and documentation changes use
the applicable checks defined in [ADR-0005](docs/adr/0005-test-driven-development.md).

Development is delivered through sequential pull requests. The next DAG slice
does not begin until the current pull request has been reviewed and merged.
Before owner review, independent personas review the slice, each finding is
evaluated against evidence, and accepted fixes receive follow-up review as
described in [ADR-0006](docs/adr/0006-sequential-pull-request-workflow.md).

All source code, identifiers, comments, documentation, ADRs, and commit
messages are written in English.

See [Code style](docs/code-style.md) for naming, formatting, and review
conventions.

## Upstream reference

The original [Google LevelDB](https://github.com/google/leveldb) implementation
is used as a behavioral oracle, file-format reference, and differential testing
target. Modern LevelDB is not a line-by-line translation.

## License

Modern LevelDB is distributed under the BSD 3-Clause License. See
[LICENSE](LICENSE).
