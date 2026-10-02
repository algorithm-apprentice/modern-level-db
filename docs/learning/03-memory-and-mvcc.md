# 03. Memory and MVCC

[Learning path](README.md) | Next: [WAL and recovery](04-wal-and-recovery.md)

Prerequisite: [internal keys](02-bytes-and-formats.md).

## Why an arena fits a memtable

Memtable entries are created together, remain at stable addresses, and are
reclaimed together when the memtable is no longer needed.
Individually allocating and freeing every entry adds allocator bookkeeping
without matching that lifetime.

An **arena** reserves blocks and advances an allocation pointer:

```text
arena block:
[entry A][entry B][alignment padding][skip-list node][unused space]
                                             allocation pointer ^
```

The current implementation uses 4 KiB normal blocks and dedicated
allocations for requests larger than 1 KiB when it needs a fallback.
It does not free individual entries. Destroying the arena releases its
owned blocks.

`memory_usage()` measures reserved block bytes plus the accounting for
block-owner pointers, not just key/value payload bytes.
That distinction matters because the engine uses it to decide when to
rotate a memtable.

Alignment makes an address suitable for a type; it does not construct that
type. Skip-list nodes and their trailing atomic links are explicitly
constructed in aligned arena storage.

## The skip list

A skip list is an ordered linked structure with additional shortcut levels:

```text
level 2: head --------------------> M --------------------> Z
level 1: head -------> D ---------> M ---------> T -------> Z
level 0: head -> A -> D -> H -> K -> M -> P -> T -> W -> Z
```

Search moves forward while it can, then drops to a lower level.
Random node heights give expected logarithmic search without tree rotations.
Here the branching factor is four and the maximum height is twelve.

The memtable stores pointers to packed arena entries as skip-list keys.
Its comparator extracts the internal key from each entry.

The concurrency contract is deliberately narrow:

- One externally serialized writer inserts nodes.
- Concurrent readers follow published links.
- Nodes are never removed while the memtable is live.

This avoids concurrent node reclamation and greatly simplifies reader
lifetime safety. It is not a general multi-writer lock-free container.

`Next` follows a level-zero link. `Prev` searches for a predecessor; it is
not a backward link. Forward and reverse traversal need not have equal cost.

## Packed entries

A memtable entry contains:

```text
varint32(internal-key length)
user key
eight-byte sequence/kind trailer
varint32(value length)
value
```

A deletion uses kind Deletion and an empty value.
Lookup uses a length-prefixed internal seek key, but it does not need a
value field.

The representation is compact, stable, and directly comparable.
Returned internal views borrow arena storage; the public `Get` copies the
found value into caller-owned storage.

## MVCC: choose a version, not a mutable slot

**Multi-version concurrency control**, or MVCC, keeps history so a reader
can choose a visibility boundary.
Modern LevelDB uses monotonically assigned sequence numbers rather than
wall-clock timestamps:

```text
color@42 Deletion
color@41 Value "blue"
color@40 Value "red"
```

For snapshot sequence `S`, seek the first entry for the key with
`sequence <= S`.

| Visible sequence | First visible entry | Result |
|---|---|---|
| 39 | None of these entries | Missing, unless an older source has history |
| 40 | Value at 40 | `"red"` |
| 41 | Value at 41 | `"blue"` |
| 42 | Deletion at 42 | Absent; stop searching |

The snapshot is a sequence registration, not a copy of the data.
Compaction must retain the versions needed by registered snapshots.

## Atomic batches and visibility

Suppose a two-operation batch gets sequences 50 and 51.
While its entries are being inserted, the published last sequence remains
49. After both insertions succeed, the engine publishes 51.

New latest reads therefore choose either the pre-batch or post-batch
boundary, not the artificial boundary between its operations.
An explicit snapshot also captures a published boundary.

This does not make separate reads a transaction:

```text
Get("a") -> concurrent batch commits -> Get("b")
```

Those calls can choose different latest sequences.
Use one explicit snapshot when several reads must share a view.
Atomic write batches do not provide conflict detection or read-modify-write
transaction semantics.

## Logical retention versus physical retention

Two different lifetimes protect readers:

| Protection | What it preserves |
|---|---|
| Snapshot registration | History required in future compacted versions |
| Memtable/version/table/cache pin | Storage already being read |

A normal iterator captures a sequence and retains its source memtables and
Version. Its old files stay available even if background work installs new
ones. An iterator created from an explicit Snapshot additionally retains
that registration.

Long-lived snapshots can retain old entries in future compaction outputs.
Long-lived iterators can retain old source files.
Both can increase resource use without copying the entire database.

## Source tour

| File | Focus |
|---|---|
| [`arena.cc`](../../src/memory/arena.cc) | Fallback allocation, alignment, and reserved-byte accounting |
| [`skiplist.h`](../../src/memory/skiplist.h) | Search, node construction, and link publication |
| [`memtable.cc`](../../src/memory/memtable.cc) | Packed entries, lookup, and checked/trusted insertion |
| [`database.cc`](../../src/engine/database.cc) | `CommitWrite`, `GetSnapshot`, and `ReleaseSnapshot` |
| [`memtable_test.cc`](../../tests/unit/memory/memtable_test.cc) | Visibility and deletion examples |
| [`public_api_posix_test.cc`](../../tests/unit/api/public_api_posix_test.cc) | Snapshot and child-handle lifetimes |

For design history, read [ADR-0015](../adr/0015-concurrent-skiplist.md),
[ADR-0017](../adr/0017-arena-backed-memtable.md), and the memory invariants
in [ADR-0060](../adr/0060-leveldb-write-path-parity.md).

## Self-check

1. Why can deleting `"color"` not simply remove its newest memtable entry?
2. Why can a reader safely hold a pointer while another thread inserts?
3. Does a 4 MiB write buffer imply a strict 4 MiB database memory limit?

<details>
<summary>Answers</summary>

1. Older values may exist in other sources, and snapshots may need them.
   A tombstone hides them for newer readers.
2. The writer publishes initialized nodes, nodes do not move or disappear,
   and a retained memtable keeps the arena alive. The publication details
   are covered in lesson 08.
3. No. There can be mutable and immutable memtables, retained readers,
   caches, metadata, and allocation slack. Rotation also checks usage at
   write boundaries rather than enforcing an allocation cap.

</details>
