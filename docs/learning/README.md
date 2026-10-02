# Learn Modern LevelDB

Modern LevelDB is small enough to study end to end, but reading its source
from the first file to the last is not an effective learning strategy.
These lessons connect storage-engine concepts to the implementation, one
question at a time.

The goal is to explain **why the engine works**, not to memorize class names.
Each core lesson includes a worked example, a bounded source tour, and
self-check questions with answers.

The project is pre-alpha. Use disposable databases for every experiment;
never use important data or modify files belonging to an open database.

## What you need first

You should recognize C++ functions, classes, containers, references, and
basic unit tests. You do not need prior database implementation experience.
The lessons introduce LSM trees, binary formats, durability, MVCC, and the
concurrency rules used here.

If `std::span`, `std::expected`, or smart-pointer ownership is unfamiliar,
read [lesson 08](08-cpp-ownership-errors-and-concurrency.md) alongside the
first source tour. This is a guide to the knowledge used by this project,
not a complete C++ or operating-systems textbook.

## Learning path

| Lesson | Main question | What you should be able to explain afterward |
|---|---|---|
| [01. The engine map](01-the-engine-map.md) | Where does a key go? | LSM trees, the major files, and the four runtime flows |
| [02. Bytes and formats](02-bytes-and-formats.md) | How are operations represented? | Fixed-width integers, varints, internal keys, batches, and validation boundaries |
| [03. Memory and MVCC](03-memory-and-mvcc.md) | How can old and new values coexist? | Arenas, skip lists, sequence visibility, snapshots, and tombstones |
| [04. WAL and recovery](04-wal-and-recovery.md) | What survives a crash? | WAL framing, file versus directory durability, MANIFEST installation, and recovery |
| [05. Tables, filters, and caches](05-tables-filters-and-caches.md) | How are sorted bytes read efficiently? | Prefix compression, restart points, Bloom filters, block compression, caching, and mmap |
| [06. Reads and iterators](06-reads-and-iterators.md) | How does the engine find the visible value? | Read-source order, level selection, merging, seek semantics, and iterator lifetimes |
| [07. Writes and compaction](07-writes-and-compaction.md) | How does the engine keep making progress? | Group commit, flushes, compaction selection, version retention, and backpressure |
| [08. C++ and concurrency](08-cpp-ownership-errors-and-concurrency.md) | What makes the implementation safe? | Ownership, explicit errors, publication, locks, condition variables, and shutdown |
| [09. Verification and performance](09-verification-and-performance.md) | What evidence supports correctness and speed? | Golden tests, models, differential tests, crash simulation, fuzzing, sanitizers, and fair measurement |
| [10. Guided labs](10-guided-labs.md) | Can I predict the engine's behavior? | A runnable API example and focused experiments with existing tests |

[The glossary](glossary.md) is a quick reference, not another prerequisite.
[The LevelDB comparison](11-leveldb-comparison.md) separates existing
capabilities, intentional differences, and possible learning-oriented
extensions. It is a proposal list, not an approved implementation roadmap.

## Choose a route

**First pass:** read lesson 01, then the explanations and examples in
lessons 02-07. Skip implementation details that do not yet make sense.
Finish with the snapshot example in lesson 10.

**Implementation pass:** follow each source-tour table in order. Read a
function together with its corresponding test before following its callees.
Use lessons 08 and 09 to explain the lifetime, synchronization, and evidence
behind each function.

**Deep pass:** do the labs, answer the self-checks without opening the
answers, and then read the linked ADRs. Study the parity and profiling ADRs
only after you understand the ordinary execution paths.

Do not read all ADRs in numerical order as your introduction. Early ADRs
describe historical implementation stages; later decisions, especially
ADR-0053 and ADR-0060, supersede some mechanisms. Current headers and tests
describe the current contract. An ADR explains the decision and its history.

## The recurring example

Most lessons use one key and illustrative sequence numbers:

```text
Put("color", "red")       -> sequence 40
GetSnapshot()            -> snapshot 40
Put("color", "blue")      -> sequence 41
Delete("color")           -> sequence 42
```

Latest reads report absence. Snapshot 40 still reads `"red"`.
The physical records may live in memory, in several tables, or in both
during a transition. None of those placements changes the required answer.
Applications do not choose these sequence numbers; the engine assigns them.

## Knowledge-to-code map

| Area | Start here | Decisions to read after the lesson |
|---|---|---|
| Public behavior and ownership | [`include/modern_leveldb/db.h`](../../include/modern_leveldb/db.h), [`src/api/api_internal.h`](../../src/api/api_internal.h) | ADR-0004, ADR-0038 |
| Binary representation | [`src/format/`](../../src/format), [`src/base/`](../../src/base) | ADR-0007, ADR-0012, ADR-0013, ADR-0016, ADR-0021 |
| In-memory indexing | [`src/memory/`](../../src/memory) | ADR-0008, ADR-0015, ADR-0017, ADR-0060 |
| Files and durability | [`src/platform/file_system.h`](../../src/platform/file_system.h), [`src/wal/`](../../src/wal) | ADR-0011, ADR-0018, ADR-0027, ADR-0029 |
| Sorted tables and caching | [`src/table/`](../../src/table), [`src/cache/`](../../src/cache) | ADR-0022 through ADR-0026, ADR-0039, ADR-0054 through ADR-0056 |
| Engine policy | [`src/engine/`](../../src/engine), [`src/metadata/`](../../src/metadata) | ADR-0030 through ADR-0037, ADR-0053, ADR-0060 |
| Engineering evidence | [`tests/`](../../tests), [`fuzz/`](../../fuzz), [`benchmarks/`](../../benchmarks) | ADR-0005, ADR-0019, ADR-0040 through ADR-0045, ADR-0052 |

Use the [ADR directory](../adr) to look up those decisions.
The [architecture](../architecture.md), [dependency DAG](../dependency-dag.md),
and [code style](../code-style.md) remain the authoritative project guides.

## Completion checkpoint

You understand the core project when you can explain, without source open:

1. Why a write reaches the WAL before it becomes visible.
2. Why a deletion can require a physical record.
3. Why a snapshot is not a copy of the database.
4. Why syncing an SSTable is not enough to install it durably.
5. Why level 0 needs different read and compaction treatment.
6. Why an iterator or cache pin can delay reclamation.
7. Why a clean benchmark or sanitizer run is evidence, not a proof.

Start with [lesson 01](01-the-engine-map.md).
