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
and bidirectional iterators while keeping engine and child-handle lifetimes
safe.

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

- [LevelDB architecture analysis](docs/architecture.md)
- [Implementation dependency DAG](docs/dependency-dag.md)
- [Architecture decision records](docs/adr/)

## Using the library

Link `modern_leveldb::modern_leveldb` and include the public facade:

```cpp
#include "modern_leveldb/db.h"

modern_leveldb::Options options;
options.create_if_missing = true;
// Snappy is the default. Compression::None and Compression::Zstd are also
// available; Zstd levels -5 through 22 are accepted.
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

`Database`, `Snapshot`, and `Iterator` are move-only handles. Snapshots and
iterators retain the underlying engine, so their storage remains valid even
if the original `Database` handle is destroyed first. See
[ADR-0038](docs/adr/0038-public-raii-api.md) for the complete contracts.

## Development

The project uses C++23, CMake, Ninja, CTest, and GoogleTest.
Compression uses private Snappy 1.3.1 and Zstd 1.5.7 dependencies. CMake reuses
parent-provided codec targets or fetches pinned source revisions; the fallback
also requires a C compiler for Zstd. No codec headers enter the public API.

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

Coverage-guided fuzzing requires a full LLVM toolchain with libFuzzer
(`brew install llvm` on macOS; select that installation's `clang`/`clang++`
with `-DCMAKE_C_COMPILER` and `-DCMAKE_CXX_COMPILER`):

```bash
cmake --preset fuzz
cmake --build --preset fuzz
ctest --preset fuzz
```

The format and stateful-engine targets use deterministic seed corpora and
explicit input, time, and memory bounds. Reproducers are retained under
`build/fuzz/fuzz/`; replay one by passing its path directly to the relevant
fuzzer executable. See [ADR-0040](docs/adr/0040-compatibility-and-crash-harness.md)
for the persistence model and its limits.

CI also measures unit-test coverage of `src/` and `include/`. Every added or
modified production line and branch must be executed by tests unless an
explicitly justified `GCOVR_EXCL_*` marker excludes it. See
[ADR-0019](docs/adr/0019-test-coverage-policy.md) for the policy and the local
coverage commands.

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
