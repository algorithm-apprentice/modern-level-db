# Appendix: Modern LevelDB, LevelDB, and Useful Next Features

[Learning path](README.md)

This comparison uses the project's pinned Google LevelDB revision
`7ee830d02b623e8ffe0b95d59a74db1e58da04c5`, not the assumption that every
LevelDB release has identical capabilities.
Modern's current public headers and engine implementation are the other
side of the comparison.

The remaining feature suggestions below are **unapproved proposals**, not
API promises or implementation tasks. Adding one still requires a concrete
requirement, an ADR, and sequential development.

## The core storage engine is already present

Modern is not missing the basic LSM pipeline:

| Capability | Pinned LevelDB | Modern LevelDB |
|---|---|---|
| Ordered binary keys and Put/Get/Delete | Yes | Yes |
| Atomic Put/Delete batches | Yes | Yes, plus an explicitly exclusive batch path |
| Ordered one-sequence batch reads | No public MultiGet | Bounded synchronous MultiGet with owning per-key results |
| Snapshots and bidirectional iteration | Yes | Yes, with RAII child lifetimes |
| WAL, MANIFEST, CURRENT, SSTable formats | Yes | Compatible core formats with golden/differential tests |
| Bloom filters | Legacy block-range layout, configurable policy | Built-in Legacy and persisted full-table layouts; automatic full filtering for the built-in bytewise path |
| None/Snappy block compression | Yes | Yes |
| Zstd block compression | Yes | Yes |
| Leveled and seek-triggered compaction | Yes | Yes |
| Concurrent API callers and grouped writes | Yes | Yes |
| Table/block caches and native mapped reads | Yes | Yes on admitted POSIX and Windows backends, with an explicit copied-read option |
| Block-cache replacement | Sharded LRU | Automatic remains LRU; explicit CLOCK is available through the checked options factory |
| Point-read table-reader reuse | Shared table-cache lookup | Same baseline, with optional bounded weak hints that default off |
| Public state/properties | Formatted string `GetProperty` | Typed owning `DatabaseStateSnapshot`; no arbitrary property strings |
| Storage-file diagnostics | `DumpFile` and `leveldbutil dump` | Versioned `modern_leveldb_tool dump` for WAL, MANIFEST, and SSTable files |
| Approximate disk usage by key range | `GetApproximateSizes` | Not exposed |
| Manual range compaction | `CompactRange` | Not implemented as a public/manual operation |
| Destroy and lossy repair helpers | `DestroyDB`, `RepairDB` | Not exposed |
| Public environment/cache/filter extension objects | Yes | Separate filesystem/clock/executor interfaces, a shared opaque block cache, and built-in Bloom configuration |

Read-path and write-path baseline parity are recorded as complete in
[ADR-0053](../adr/0053-leveldb-read-path-parity.md) and
[ADR-0060](../adr/0060-leveldb-write-path-parity.md).
That does not mean identical performance on every workload or identical
source APIs.
[ADR-0091](../adr/0091-native-get-cost-model-parity.md) separately records
alignment of generated read work: pin transfers, view forwarding, complete-word
operations, and decoding of validated index handles. Algorithmic similarity
alone does not establish the same calls, ownership cost, or timing.

The full-table filter uses optional metadata, not a replacement for the core
SST format. Native can read its data while ignoring the unknown filter;
it does not receive the new exclusion benefit. See
[lesson 05](05-tables-filters-and-caches.md#bloom-filters) for the layout and
comparator-equality requirements.

## Intentional differences are not automatically gaps

Modern uses explicit expected-based errors, typed byte views, owned
comparators, and RAII snapshots/iterators.
Children can retain the engine after the original Database handle is
destroyed. Source/ABI compatibility with LevelDB is a non-goal.

Modern also retains named safety/durability differences, including default
WAL-creation namespace durability and explicit sticky uncertain-write
handling. Production table reads verify stored block checksums rather than
exposing LevelDB's per-read `verify_checksums` switch.

Filesystem, cache, clock, and executor injection are available through public
options, with supplied owners retained for the complete engine/child lifetime.
This is not a combined Env or arbitrary cache/filter plugin framework; see
[API reference](../reference/api-and-options.md#supplying-runtime-dependencies)
and [ADR-0079](../adr/0079-public-runtime-dependency-injection.md).

Snappy, Zstd, snapshots, Bloom filters, and compaction should not be proposed
as new features: they are already implemented.

## Prioritize learning value

For this project's educational purpose, a useful extension should make a
hidden invariant or transition observable without making the engine much
harder to understand.

| Priority | Proposal | Knowledge made visible | Smallest useful boundary |
|---|---|---|---|
| Next | Public deterministic flush | WAL/memtable/table transitions and installation barriers | Reuse the internal flush mechanism with explicit blocking/error semantics |
| Next, separate task | Manual range compaction | Input selection, tombstone retention, and physical reclamation | Define scheduling/range/completion contracts before implementing |
| Lower priority | Approximate range disk sizes | Logical versus compressed physical space | Clearly approximate SSTable accounting; not exact live-value size |

### Typed state inspection is now implemented

`Database::GetState` now exposes per-level file counts/bytes, the published
sequence, explicit snapshot retention, write-queue depth, mutable/immutable
memtable arena reservations, protected output numbers, accepted background
work, and the recorded sticky error.
It answers questions such as:

```text
Why did this write slow down?
Why did deleting half the keys not halve disk usage?
Why are old tables still present?
Did this workload leave maintenance debt?
```

Those facts are more useful to a learner than many tuning knobs. The value is
an owning on-demand snapshot, not a metrics stream, file inspector,
background-progress estimate, or total process-memory report.

Ordinary profiling reports and CPU capture remain available.
Fine-grained read diagnostics were retired under
[ADR-0076](../adr/0076-textbook-source-composition.md).
The production state API does not add expensive read instrumentation.

### Read-only storage diagnostics are now implemented

`modern_leveldb_tool dump` decodes canonical WAL, MANIFEST, and SSTable files
through existing production readers. It shows escaped binary keys and values,
batch sequences, VersionEdit fields, and visible table history.
This directly connects lessons 02, 04, and 05 without adding online engine
state.

Use it on a closed disposable database or an already consistent offline copy.
Blindly copying an actively changing database directory does not create a
consistent backup.
Encountered corruption remains a non-success result even when safe partial
text is available. The text is not restore input, a whole-file verification
certificate, or automatic repair.

### Flush is smaller than manual compaction

`DatabaseEngine::FlushMemTable` already supplies an internal flush barrier.
Making it public still needs a precise ownership, error, and waiting
contract, but can reuse existing behavior.

Manual range compaction is not just exporting that method.
It needs a selection/scheduling path and a definition of what completion
means while writes and snapshots remain active.
Neither operation can promise immediate reclamation of pinned old files.

### Cache capacity is a budget

Capacity is already configurable through the shared public cache factories.
The checked options factory also selects LRU or CLOCK; Automatic remains LRU.
The retained block charge is not RSS: pinned blocks, mapped pages, table
metadata, memtables, and allocator/cache metadata complicate a hard-memory
claim. A policy with cheaper hit maintenance can still lose through a worse
miss pattern. Do not turn the explicit CLOCK or reader-reuse options into
default recommendations without the relevant workload evidence.

## Features to defer

| Feature | Reason to defer for the current learning scope |
|---|---|
| Iterator bounds | Still require a concrete caller and precise comparator/range semantics; one-sequence MultiGet is already implemented |
| Live checkpoint/backup | Requires a consistent file/WAL boundary; not a primitive in the pinned LevelDB DB API |
| Lossy repair | Large correctness/data-loss contract; an inspector and corruption exercises provide a safer first step |
| Additional Windows platforms and storage | Native x64/MSVC local-NTFS opening, LevelDB-aligned mappings, diagnostics, benchmarks and profiling are implemented; strict namespace durability, other architectures/toolchains and non-NTFS/network storage remain outside the admitted scope |
| Transactions, column families, merge operators | Expand the storage model and are explicit current non-goals |
| TTL and range deletion | Add visibility/retention semantics, not just convenience methods |
| Parallel compaction or asynchronous writes | Introduce scheduling, ordering, resource, and shutdown complexity before a measured need |
| SQL, replication, remote storage | Change the product category rather than illuminate this small embedded engine |

For performance work, prefer a measured, bounded investigation.
The retained production `mixed50` regression in
[ADR-0060](../adr/0060-leveldb-write-path-parity.md) is an example of a valid
research question, not a reason to invent another feature or discard
unfavorable measurements.

## A useful next-step sequence

For study, finish the learning path and existing labs first.
If extending the engine afterward, prefer one deterministic maintenance
experiment, beginning with a precisely designed manual compaction or public
flush boundary.
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
- [Read-diagnostic retirement](../adr/0076-textbook-source-composition.md)
- [Platform support and durability](../reference/platform-support-and-durability.md)
