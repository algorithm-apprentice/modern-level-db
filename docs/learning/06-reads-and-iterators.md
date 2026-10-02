# 06. Reads and Iterators

[Learning path](README.md) | Next: [Writes and compaction](07-writes-and-compaction.md)

Prerequisites: [MVCC](03-memory-and-mvcc.md) and
[table lookup](05-tables-filters-and-caches.md).

## A Get is a visibility decision

Before searching, the engine chooses a sequence:

- An explicit Snapshot supplies its registered sequence.
- Otherwise the read captures the published last sequence at its beginning.

It pins the mutable memtable, optional immutable memtable, and current
Version under the database mutex. Slow lookup proceeds without holding
that mutex, while the pins preserve its sources.
Pin release and seek-statistics updates happen under the mutex again.

This short locked setup is different from locking the entire database for
every disk read.

## Missing and deleted are not interchangeable

Every source answers one of three logical outcomes:

| Outcome | Next action |
|---|---|
| Missing | Search the next source |
| Value | Return the value |
| Deletion | Return absence; do not search older sources |

Suppose the mutable memtable contains `color@42 Deletion`, while L2 still
contains `color@40 "red"`.
At sequence 42, continuing after the tombstone would resurrect `"red"`.
At sequence 40, the tombstone is invisible, so older sources may be needed.

Errors are a fourth, separate outcome. A corrupt table is not a missing key.

## Why source order works

The search order is mutable, immutable, L0, and then deeper levels.
Memtable generations contain newer update intervals than their older
sources.

L0 is special: its files can overlap. The engine collects candidate ranges
and searches them in descending file-number order, newest first.
The Version's stored L0 vector order is not itself the lookup chronology.

For nonzero levels, files have ordered, non-overlapping internal-key
ranges. The engine binary-searches for a candidate using the
snapshot-dependent internal lookup key, then checks its user-key lower
bound.
Adjacent files can have boundary versions of the same user key; do not
replace this search with a user-key-only shortcut.

At most one candidate per deeper level is searched by this point-read path.

```text
mutable lookup
    -> immutable lookup
    -> overlapping L0 candidates, newest first
    -> candidate in L1
    -> candidate in L2
    -> ...
```

Within a table:

```text
table cache -> index seek -> optional Bloom test
            -> data-block cache/read -> block seek -> visible entry
```

## Two public result shapes

The convenient overload returns:

```text
Result<optional<vector<byte>>>

error                 -> storage/API failure
success + empty       -> key absent
success + value       -> key present, possibly with zero-length value
```

The reusable overload returns `Result<bool>` and fills a caller-owned vector
only when it finds a value. Missing/deleted keys leave that vector unchanged.
Errors leave it valid with unspecified contents.

An unchanged buffer after a miss is therefore not the result of the new
read. Check the returned boolean before interpreting its contents.
Reusing capacity can avoid repeated result-buffer allocation without
returning a borrowed pointer into internal storage.

## Iteration has two layers

An **internal iterator** merges sorted physical streams:

```text
mutable entries --------\
immutable entries -------\
each L0 table ------------> merging iterator -> internal-key order
concatenated L1 files ----/
concatenated L2 files ---/
```

L0 needs separate merged children because its files overlap.
Nonzero levels can concatenate their ordered files.
The implementation selects among children directly; do not assume every
merging iterator uses a heap.

The **DB iterator** turns internal history into user-visible keys:

1. Ignore entries newer than its captured sequence.
2. Resolve the newest visible version for each user key.
3. Suppress deletions and older versions.
4. Yield each visible user key once.

At snapshot 40, the running example yields `"color" -> "red"`.
At sequence 42, it yields no `"color"` entry.

## Seek and direction changes

`Seek(target)` positions at the first visible user key not less than the
target under the configured comparator.
It does not mean exact-match lookup.

For visible keys `"a"`, `"c"`, and `"f"`:

```text
Seek("b") -> "c"
Seek("z") -> invalid, successful end-of-range
```

Every positioning operation returns `Status`.
Invalid after success can mean normal exhaustion; invalid after failure
means an error occurred. Checking `valid()` alone loses that distinction.
Call `Next`, `Prev`, `key`, and `value` only at a valid position.

Reverse iteration is not simply forward iteration with reversed links.
For each user key, the engine walks internal history in the opposite order
and retains the newest visible value. Direction changes must reposition
children so they do not repeat or skip a user key.

The reverse DB iterator saves key/value bytes because its internal iterator
can already be positioned before the returned user's entries.

## Lifetimes and range scans

An iterator starts invalid and retains its engine and source lifetimes.
Destroying the original public Database handle does not invalidate that
iterator. It also does not close the underlying engine while children
remain.

`key()` and `value()` are borrowed views valid only until the iterator moves
or is destroyed. Copy them before retaining them across `Next` or `Prev`.

The public API currently has no lower/upper-bound iterator options.
For a half-open range `[start, limit)`, a caller can seek to `start`, then
stop when the key is not less than `limit` under the same comparator.
Byte-prefix shortcuts are not generally valid for arbitrary comparators.

## Reads can influence compaction

A point read that needs another file can charge an earlier file's seek
budget. Iterators also sample read bytes and charge overlapping files.
An exhausted budget can request compaction even when size scores are low.

This is read-amplification feedback, not an exact count of device seeks.
Cache hits and mapped pages complicate the relationship to physical I/O.

## Source tour

| File | Focus |
|---|---|
| [`database.cc`](../../src/engine/database.cc) | `Get`, `ReadSources`, and `NewIterator` |
| [`lookup.cc`](../../src/engine/lookup.cc) | `LookupValue`, L0 ordering, and `LevelCandidate` |
| [`iterators.cc`](../../src/engine/iterators.cc) | Merging and level concatenation |
| [`db_iterator.cc`](../../src/engine/db_iterator.cc) | Visibility, reverse state, and direction changes |
| [`seek_statistics.cc`](../../src/engine/seek_statistics.cc) | Seek-budget feedback |
| [`db_iterator_test.cc`](../../tests/unit/engine/db_iterator_test.cc) | Seeks, tombstones, and direction changes |

Follow with [ADR-0030](../adr/0030-point-reads.md),
[ADR-0031](../adr/0031-iterators.md), and the completed baseline in
[ADR-0053](../adr/0053-leveldb-read-path-parity.md).

## Self-check

1. What must a latest read do after finding a visible deletion in memory?
2. Why are two latest Gets not necessarily a consistent two-key read?
3. Can you save an iterator's `ByteView` and use it after advancing?

<details>
<summary>Answers</summary>

1. Return absence immediately, not search tables for an old value.
2. Each call can capture a different published sequence. One explicit
   Snapshot is needed for a shared read view.
3. No. Copy the bytes into owned storage first.

</details>
