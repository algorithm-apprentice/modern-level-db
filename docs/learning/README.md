# Learn Modern LevelDB

Modern LevelDB is small enough to study end to end, but reading its source
from the first file to the last is not an effective learning strategy.
These lessons develop storage-engine models and arguments before mapping
them to the implementation, one problem at a time.

The goal is to explain **why the engine works**, not to memorize class names.
Each core lesson separates its problem, model, derivation, correctness,
costs, and exercises from the final bounded source tour. Treat model
assumptions as part of the result, not as universal database guarantees.

Lessons are organized from the point of view of a component's consumer:

```text
problem -> observable specification -> model and assumptions
        -> derivation -> correctness and cost -> exercises
        -> implementation case study and evidence
```

A consumer may be an application or another internal component. This lets a
reader treat one layer as a black box before choosing to open the next layer.
The curriculum structure is recorded in
[ADR-0072](../adr/0072-consumer-oriented-learning.md).

The project is pre-alpha. Use disposable databases for every experiment;
never use important data or modify files belonging to an open database.
Use the current [getting-started guide](../reference/getting-started.md) for
toolchain prerequisites and
[platform support and durability](../reference/platform-support-and-durability.md)
before running filesystem-backed examples.

## What you need first

You should recognize C++ functions, classes, containers, references, and
basic unit tests. You do not need prior database implementation experience.
The [reasoning toolkit](00-reasoning-toolkit.md) introduces the loop,
probability, cost, and failure-model arguments used later. Optional executable
teaching models use standard-library Python 3.10 or newer; they do not open
a database or replace the production parsers.
The lessons introduce LSM trees, binary formats, durability, MVCC, and the
concurrency rules used here.

If `std::span`, `std::expected`, or smart-pointer ownership is unfamiliar,
read [lesson 08](08-cpp-ownership-errors-and-concurrency.md) alongside the
first source tour. This is a guide to the knowledge used by this project,
not a complete C++ or operating-systems textbook.

## Learning path

| Lesson | Main question | What you should be able to explain afterward |
|---|---|---|
| [00. Reasoning toolkit](00-reasoning-toolkit.md) | What would make an explanation convincing? | Specifications, loop invariants, termination, cost units, expected/amortized reasoning, and counterexamples |
| [01. The engine map](01-the-engine-map.md) | Where does a key go? | Specify visible history, derive the log/index split, and separate logical, lifetime, and durable-installation proofs |
| [02. Bytes and formats](02-bytes-and-formats.md) | How are operations represented? | Derive integer encodings and internal order, prove cursor commit, and distinguish byte compatibility from round trips |
| [03. Memory and MVCC](03-memory-and-mvcc.md) | How can old and new values coexist? | Prove lower-bound visibility and publication, derive expected skip-list costs, and recognize the fixed-height limit |
| [04. WAL and recovery](04-wal-and-recovery.md) | What survives a crash? | State a failure model, derive framing costs, and prove content/name/metadata dependencies across crash cuts |
| [05. Tables, filters, and caches](05-tables-filters-and-caches.md) | How are sorted bytes read efficiently? | Derive restart/Bloom/cache tradeoffs with explicit cost units and retain borrowed storage correctly |
| [06. Reads and iterators](06-reads-and-iterators.md) | How does the engine find the visible value? | Justify source dominance, internal-key routing, merged order, direction changes, and one-sequence batch reads |
| [07. Writes and compaction](07-writes-and-compaction.md) | How does the engine keep making progress? | Analyze grouped barriers, prove snapshot/tombstone retention, and account for unfinished maintenance |
| [08. C++ and concurrency](08-cpp-ownership-errors-and-concurrency.md) **(advanced)** | What makes the implementation safe? | Construct owner/borrow, move, publication, lock-restoration, and shutdown arguments without assuming cleanup is free |
| [09. Verification and performance](09-verification-and-performance.md) **(advanced)** | What evidence supports correctness and speed? | Build independent oracles and falsifiable claims, calculate paired summaries, and design controlled measurements |
| [10. Guided labs](10-guided-labs.md) | Can I solve an unfamiliar variant? | Submit predictions, counterexamples, safe observations, transfer challenges, and an independently defended capstone |

### Executable study units

After lesson 05's representation introduction, work through
[searching a prefix-compressed block](05a-restart-search.md). Derive its
fresh lower bound, prove the fallback-aware binary-search invariant, account
for the k+1 replay boundary, and attempt the design/transfer exercises before
opening the source. Its executable model checks worked tables against an
independent lower-bound oracle.

After lesson 07's retention argument, run the
[one-key compaction model](examples/retention.py). Explain why its
oldest-view and deeper-history premises are necessary, then construct a
new history before reading the tests. Neither model parses persistent
files or substitutes finite enumeration for the written proof.

[The glossary](glossary.md) is a quick reference, not another prerequisite.
[The concept-to-code index](concept-index.md) is the maintenance map from a
storage concept to its current contract, implementation entry points, tests,
and decision history.

### Advanced deep dives and appendices

| Reading | Use it when |
|---|---|
| [11. Comparing engines](11-leveldb-comparison.md) **(appendix)** | Separating semantic, representation, generated-work, and workload claims before transferring a design |
| [12. Native filesystem lifecycles](12-native-filesystem-lifecycles.md) **(advanced)** | Proving terminal-event and rollback safety, error priority, and the limits of mapping-count budgets |

The comparison appendix also retains an explicitly unapproved proposal list,
not an implementation roadmap. Lesson 12 is a maintainer deep dive, not a
first-pass prerequisite.

## Choose a route

**First pass:** use lesson 00 when a reasoning tool is unfamiliar, then
read the problem/model, derivation, proof, and worked examples in 01-07.
Attempt exercises before opening answer checks or source tours. Finish with
the independent predictions and runnable assignments in lesson 10.

**Implementation pass:** follow each source-tour table in order. Read a
function together with its corresponding test before following its callees.
Use lessons 08 and 09 to explain the lifetime, synchronization, and evidence
behind each function. For a concrete change, start from the
[concept-to-code index](concept-index.md) and follow the
[document-first development workflow](../development/document-first-development.md).

**Deep pass:** solve the design/transfer exercises and labs before opening
their hints, and then read the linked ADRs. Study the parity and profiling ADRs
only after you understand the ordinary execution paths. For native platform
work, read lesson 12 after lessons 04, 05, and 08.

Do not read all ADRs in numerical order as your introduction. Early ADRs
describe historical implementation stages; later decisions, especially
[ADR-0053](../adr/0053-leveldb-read-path-parity.md) and
[ADR-0060](../adr/0060-leveldb-write-path-parity.md), supersede some
mechanisms. Current headers, tests, and reference documents describe the
current contract. An ADR explains the decision and its history.

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

Use the [ADR index](../adr/README.md) to search decisions by topic and
lifecycle. The direct record links below are useful starting points, not a
replacement for the current reference documents.

This table gives a broad repository orientation. The maintained
[concept-to-code index](concept-index.md) provides the finer change-oriented
map to implementation entry points and representative tests.

| Area | Start here | Decisions to read after the lesson |
|---|---|---|
| Public behavior and ownership | [`include/modern_leveldb/db.h`](../../include/modern_leveldb/db.h), [`src/api/api_internal.h`](../../src/api/api_internal.h) | [ADR-0004](../adr/0004-errors-ownership-and-runtime.md), [ADR-0038](../adr/0038-public-raii-api.md) |
| Binary representation | [`src/format/`](../../src/format), [`src/base/`](../../src/base) | [ADR-0007](../adr/0007-checksum-and-hash-contracts.md), [ADR-0012](../adr/0012-internal-key-format.md), [ADR-0013](../adr/0013-wal-record-format.md), [ADR-0016](../adr/0016-write-batch-format.md), [ADR-0021](../adr/0021-manifest-version-edits.md) |
| In-memory indexing | [`src/memory/`](../../src/memory) | [ADR-0008](../adr/0008-monotonic-arena.md), [ADR-0015](../adr/0015-concurrent-skiplist.md), [ADR-0017](../adr/0017-arena-backed-memtable.md), [ADR-0060](../adr/0060-leveldb-write-path-parity.md) |
| Files and durability | [`src/platform/file_system.h`](../../src/platform/file_system.h), [`src/wal/`](../../src/wal) | [ADR-0011](../adr/0011-filesystem-contracts.md), [ADR-0018](../adr/0018-wal-stream-io.md), [ADR-0027](../adr/0027-version-set.md), [ADR-0029](../adr/0029-database-recovery.md), [ADR-0056](../adr/0056-leveldb-table-mmap-parity.md), [ADR-0064](../adr/0064-windows-filesystem-and-delivery.md), [ADR-0068](../adr/0068-windows-mapped-read-parity.md) |
| Sorted tables and caching | [`src/table/`](../../src/table), [`src/cache/`](../../src/cache) | [ADR-0022](../adr/0022-sstable-block-format.md) through [ADR-0026](../adr/0026-table-cache.md), [ADR-0039](../adr/0039-sstable-block-compression.md), [ADR-0054](../adr/0054-leveldb-cache-parity.md) through [ADR-0056](../adr/0056-leveldb-table-mmap-parity.md) |
| Engine policy | [`src/engine/`](../../src/engine), [`src/metadata/`](../../src/metadata) | [ADR-0030](../adr/0030-point-reads.md) through [ADR-0037](../adr/0037-database-engine.md), [ADR-0053](../adr/0053-leveldb-read-path-parity.md), [ADR-0060](../adr/0060-leveldb-write-path-parity.md) |
| Engineering evidence | [`tests/`](../../tests), [`fuzz/`](../../fuzz), [`benchmarks/`](../../benchmarks) | [ADR-0005](../adr/0005-test-driven-development.md), [ADR-0019](../adr/0019-test-coverage-policy.md), [ADR-0040](../adr/0040-compatibility-and-crash-harness.md) through [ADR-0045](../adr/0045-fixed-work-write-profiling.md), [ADR-0052](../adr/0052-cross-engine-read-gap-decomposition.md) |

The [architecture](../architecture.md),
[development guides](../development/README.md), and
[code style](../development/code-style.md) remain the authoritative project
guides.

## Completion checkpoint

Use these as an initial checkpoint, without source open:

1. Why a write reaches the WAL before it becomes visible.
2. Why a deletion can require a physical record.
3. Why a snapshot is not a copy of the database.
4. Why syncing an SSTable is not enough to install it durably.
5. Why level 0 needs different read and compaction treatment.
6. Why an iterator or cache pin can delay reclamation.
7. Why a clean benchmark or sanitizer run is evidence, not a proof.

Then derive a restart-search bound, explain a Bloom-model assumption,
calculate a paired cost example, and defend the independent
[lab capstone](10-guided-labs.md#capstone-explain-a-complete-lifecycle).
Being able to repeat the explanations is not enough: solve a changed
assumption or unfamiliar representation and show where your proof changes.
The course supplies that assessment structure; it does not claim measured
learning effectiveness without reader evaluation.

For the platform-maintainer deep pass, also explain why native resource
cleanup must reach a terminal state before releasing a borrowed buffer,
mapped view, lock identity, or capacity slot.

Start with [lesson 01](01-the-engine-map.md).
