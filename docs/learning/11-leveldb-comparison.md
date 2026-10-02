# Modern LevelDB, LevelDB, and Useful Next Features

[Learning path](README.md)

This comparison uses the project's pinned Google LevelDB revision
`7ee830d02b623e8ffe0b95d59a74db1e58da04c5`, not the assumption that every
LevelDB release has identical capabilities.
Modern's current public headers and engine implementation are the other
side of the comparison.

The feature suggestions below are **unapproved proposals**, not API
promises or implementation tasks. Adding one still requires a concrete
requirement, an ADR, and sequential development.

## The core storage engine is already present

Modern is not missing the basic LSM pipeline:

| Capability | Pinned LevelDB | Modern LevelDB |
|---|---|---|
| Ordered binary keys and Put/Get/Delete | Yes | Yes |
| Atomic Put/Delete batches | Yes | Yes, plus an explicitly exclusive batch path |
| Snapshots and bidirectional iteration | Yes | Yes, with RAII child lifetimes |
| WAL, MANIFEST, CURRENT, SSTable formats | Yes | Compatible core formats with golden/differential tests |
| Bloom filters | Yes, configurable policy | Yes, optional built-in Bloom configuration |
| None/Snappy/Zstd block compression | Yes | Yes |
| Leveled and seek-triggered compaction | Yes | Yes |
| Concurrent API callers and grouped writes | Yes | Yes |
| Table/block caches and POSIX mapped reads | Yes | Yes, with an explicit copied-read option |
| Public properties/statistics | `GetProperty` | No equivalent public API |
| Approximate disk usage by key range | `GetApproximateSizes` | Not exposed |
| Manual range compaction | `CompactRange` | Not implemented as a public/manual operation |
| Destroy and lossy repair helpers | `DestroyDB`, `RepairDB` | Not exposed |
| Public environment/cache/filter extension objects | Yes | Narrower facade; engine injection remains internal |

Read-path and write-path baseline parity are recorded as complete in
[ADR-0053](../adr/0053-leveldb-read-path-parity.md) and
[ADR-0060](../adr/0060-leveldb-write-path-parity.md).
That does not mean identical performance on every workload or identical
source APIs.

## Intentional differences are not automatically gaps

Modern uses explicit expected-based errors, typed byte views, owned
comparators, and RAII snapshots/iterators.
Children can retain the engine after the original Database handle is
destroyed. Source/ABI compatibility with LevelDB is a non-goal.

Modern also retains named safety/durability differences, including default
WAL-creation namespace durability and explicit sticky uncertain-write
handling. Production table reads verify stored block checksums rather than
exposing LevelDB's per-read `verify_checksums` switch.

Filesystem, cache, clock, and executor injection exist internally for
concrete engine/test callers. Exposing all of those as a general plugin
framework would add public contracts, not implement a missing LSM concept.

Snappy, Zstd, snapshots, Bloom filters, and compaction should not be proposed
as new features: they are already implemented.

## Prioritize learning value

For this project's educational purpose, a useful extension should make a
hidden invariant or transition observable without making the engine much
harder to understand.

| Priority | Proposal | Knowledge made visible | Smallest useful boundary |
|---|---|---|---|
| First | Typed database-state/statistics snapshot | Levels, retained memory/history, write stalls, maintenance progress/errors | Copy selected state under the engine mutex; no generic metrics framework |
| First | Offline WAL/MANIFEST/SSTable inspector | Binary formats, sequences, checksums, block routing, metadata edits | Read-only tools over disposable, closed database files |
| Next | Public deterministic flush | WAL/memtable/table transitions and installation barriers | Reuse the internal flush mechanism with explicit blocking/error semantics |
| Next, separate task | Manual range compaction | Input selection, tombstone retention, and physical reclamation | Define scheduling/range/completion contracts before implementing |
| When experiments need it | Public block-cache capacity | Eviction, working sets, pins, and memory/read tradeoffs | A per-database size option, not arbitrary shared cache plugins |
| Lower priority | Approximate range disk sizes | Logical versus compressed physical space | Clearly approximate SSTable accounting; not exact live-value size |

### State/statistics first

A small initial state report could expose per-level file counts/bytes,
memtable usage, registered-snapshot state, current maintenance state, and
the sticky background error.
It would answer questions such as:

```text
Why did this write slow down?
Why did deleting half the keys not halve disk usage?
Why are old tables still present?
Did this workload leave maintenance debt?
```

Those facts are more useful to a learner than many tuning knobs.
Some state already exists; completed-work counters would require new
accounting. Do not claim the public API merely needs to forward an existing
complete statistics implementation.

The project already has a separate read-diagnostics executable and retained
profiling reports. Start there for detailed read counters.
A production state API should not add expensive instrumentation to every
hot-path operation just to reproduce those diagnostics.

### An inspector is not automatic repair

An inspector could show escaped binary keys, sequence/kind trailers, batch
contents, physical WAL fragments, table block offsets, restart points, and
MANIFEST file edits.
This directly connects lessons 02, 04, and 05.

Prefer standalone read-only inspection of a closed disposable database or
an already consistent offline copy.
Blindly copying an actively changing database directory does not create a
consistent backup.
Report corruption explicitly; never silently turn damaged data into a
successful repair.

### Flush is smaller than manual compaction

`DatabaseEngine::FlushMemTable` already supplies an internal flush barrier.
Making it public still needs a precise ownership, error, and waiting
contract, but can reuse existing behavior.

Manual range compaction is not just exporting that method.
It needs a selection/scheduling path and a definition of what completion
means while writes and snapshots remain active.
Neither operation can promise immediate reclamation of pinned old files.

### Cache capacity is a budget

Expose capacity only when a reproducible cache experiment or application
needs it. Preserve the default and document that pinned blocks, mapped
pages, table metadata, and memtables lie outside a simple hard-memory claim.

## Features to defer

| Feature | Reason to defer for the current learning scope |
|---|---|
| MultiGet and iterator bounds | Useful application-facing extensions, but first establish a concrete caller and common-snapshot/comparator semantics |
| Live checkpoint/backup | Requires a consistent file/WAL boundary; not a primitive in the pinned LevelDB DB API |
| Lossy repair | Large correctness/data-loss contract; an inspector and corruption exercises provide a safer first step |
| Windows database filesystem | A substantial platform/durability project; portable components already build, but database opening currently needs the POSIX backend |
| Transactions, column families, merge operators | Expand the storage model and are explicit current non-goals |
| TTL and range deletion | Add visibility/retention semantics, not just convenience methods |
| Parallel compaction or asynchronous writes | Introduce scheduling, ordering, resource, and shutdown complexity before a measured need |
| SQL, replication, remote storage | Change the product category rather than illuminate this small embedded engine |

For performance work, prefer a measured, bounded investigation.
The retained production `mixed50` regression in ADR-0060 is an example of a
valid research question, not a reason to invent another feature or discard
unfavorable measurements.

## A useful next-step sequence

For study, finish the learning path and existing labs first.
If extending the engine afterward, prefer one observability or inspection
feature, then one deterministic maintenance experiment.
Do not implement every row above simply to match an API checklist.

The decision rule remains [need-driven simplicity](../adr/0010-need-driven-simplicity.md):
identify the caller, invariant, lifetime, failure model, and verification
before adding a capability.

## Comparison sources

- [Pinned LevelDB database API](https://github.com/google/leveldb/blob/7ee830d02b623e8ffe0b95d59a74db1e58da04c5/include/leveldb/db.h)
- [Pinned LevelDB options](https://github.com/google/leveldb/blob/7ee830d02b623e8ffe0b95d59a74db1e58da04c5/include/leveldb/options.h)
- [Modern database API](../../include/modern_leveldb/db.h)
- [Modern options](../../include/modern_leveldb/options.h)
- [Internal engine and existing flush barrier](../../src/engine/database.h)
- [Existing read diagnostics](../../src/engine/read_diagnostics.h)
- [Goals and non-goals](../../README.md)
