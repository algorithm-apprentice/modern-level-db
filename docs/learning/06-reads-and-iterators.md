# 06. Reads and Iterators

[Learning path](README.md) | Next: [Writes and compaction](07-writes-and-compaction.md)

Prerequisites: [MVCC](03-memory-and-mvcc.md) and
[table lookup](05-tables-filters-and-caches.md).

## Lesson map

| Question | Answer |
|---|---|
| What problem is this lesson about? | Finding the newest visible logical value across several ordered memory and file sources |
| Who consumes the read path? | Public Get and Iterator calls |
| What interface matters first? | Get outcomes, Iterator seek/next/prev, Snapshot selection, and borrowed/owned output |
| What implementation will we open? | Source pins, mutable/immutable order, L0 chronology, level search, merging, and direction changes |
| What should you be able to do afterward? | Predict a read result from versions/tombstones and explain why source order is correct |

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

### Make the representation invariant explicit

For a fixed user key and read sequence, let each source's candidate be its
newest visible value or deletion, if any. The layout must ensure that the
first candidate in search order is not older than a candidate that should
win later. This **per-key source dominance** is maintained by write
generations and overlap-complete maintenance, not by sortedness alone.

Under that invariant, stopping is correct: Missing supplies no decision;
the first Value is the newest visible value; the first Deletion hides every
older value. An error is never a proof that a source is empty.

A counterexample shows the missing premise. Put an old visible record in a
higher-priority source and a newer visible record for the same key in a
later source. Both sources can be internally sorted, yet first-match lookup
returns the wrong value. Level topology and the compaction inclusion rules
must preserve more than individual file order.

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

### Prove the deeper-level candidate selection

The binary search chooses the first file whose largest **internal** key is
not before the snapshot target. Every excluded file contains only keys
before that target. The candidate's smallest user key must then be checked:
if it lies after the requested user key, later files cannot supply that key
either.

For one user key, file A might contain sequences 100 through 80 and adjacent
file B sequences 40 through 10. A snapshot target at 50 must skip A and
select B, even though both files' user-key bounds include that same key.
A user-key-only binary search can choose A incorrectly.

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
table cache -> optional full-table filter -> index seek
            -> optional legacy range filter -> data-block cache/read
            -> block seek -> visible entry
```

The filter checks are alternative layouts selected from table metadata, not
two required passes. A full-table negative can avoid the index seek; a legacy
range filter needs the selected data-block handle first. Tables without
filters continue through the real lookup.

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

## Several keys need one read view

Two latest Gets can straddle an atomic write batch:

```text
Get(left) -> old
writer atomically changes left and right
Get(right) -> new
```

Each call is correct, but the pair is not a shared read view.
An explicit Snapshot can coordinate separate Gets.
`Database::MultiGet` instead retains one sequence for the complete ordered
batch and uses it for every key, including duplicate positions. In this
example, it observes the state before or after the atomic update, not a
mixture of those two states.

The results own their bytes and distinguish per-key absence from per-key
errors. Bounded readers finish before the call returns. Joining workers
protects their borrowed input/result lifetimes; retaining one sequence
protects logical consistency. Neither proof replaces the other.
See [lesson 08](08-cpp-ownership-errors-and-concurrency.md#batch-lifetime-and-read-consistency-are-separate-proofs)
for the startup/exception proof and the
[API reference](../reference/api-and-options.md) for bounds and result shapes.

A batch coordinates ordinary reads; it is not a promise of vectorized I/O,
lower latency, or better performance for a single key.

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
`AppendLevelIterators` shares this composition rule between ordinary reads and
compaction; each caller still selects its own sources and cache options.
The implementation selects among children directly; do not assume every
merging iterator uses a heap.

### Derive merge correctness and cost

Suppose each of m child streams is sorted. Every unseen entry in a stream
is at least its current head. The minimum valid head is therefore a global
minimum. Emit it, advance only that child, and repeat. This invariant proves
the merged order; it does not yet remove old user versions or tombstones.
An errored child must propagate failure, not masquerade as an exhausted one.

Choosing the minimum by scanning all heads costs O(m) comparisons per
selection. A heap can reduce the selection model to O(log m) after building
its state, but seeking all children and changing direction still have costs.
Variable key comparison, tiny m, allocation, and cache behavior affect the
real tradeoff.

Count streams, not table files: many disjoint files in one nonzero level
share one concatenating stream, while overlapping L0 files need separate
children. A large directory does not automatically imply a large merge heap.

For multiple children, each child caches its validity and borrowed key view.
Only moving that child refreshes the cache; choosing the next stream does not
repeatedly call through its iterator interface. Key views still expire when
their child moves. A single child passes through without another iterator layer.
The merger clears its selected position before fallible child movement and
publishes a new selection only after success, including during direction changes.

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

The saved key also supplies reusable storage for an internal seek target:
the requested user key followed by the iterator's fixed sequence and seek kind.
Copy the user key before appending the trailer, including when the request
borrows the iterator's own saved key. Capacity survives subsequent clears.
Movement invalidates the public position before changing storage or children,
so an exception cannot expose a stale key/value pair as a valid position.

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

## Exercises: justify the whole read, not one container

1. What must a latest read do after finding a visible deletion in memory?
2. Why are two latest Gets not necessarily a consistent two-key read?
3. Can you save an iterator's `ByteView` and use it after advancing?
4. What can happen if overlapping L0 files are searched oldest first?
5. Would joining several ordinary latest-read workers alone make their
   results a shared read view?
6. **Counterexample:** construct two sorted sources whose priority violates
   per-key dominance. Why is sorting each source insufficient?
7. **Trace:** reproduce the two-file snapshot-50 selection. Which internal
   bound excludes the newer-only file?
8. **Proof:** prove that the minimum child head is a global minimum, then
   identify what fails if one child returns stale cached head metadata.
9. **Cost:** compare a linear selector and heap for two versus many streams.
   Which input size and costs must a meaningful experiment hold fixed?
10. **Transfer:** a new engine has overlapping ranges at every level.
    Which proof permits one candidate per deeper level here, and why can
    that shortcut not be imported unchanged?

<details>
<summary>Answers</summary>

1. Return absence immediately, not search tables for an old value.
2. Each call can capture a different published sequence. Coordinate them
   with one explicit Snapshot or use a one-sequence MultiGet batch.
3. No. Copy the bytes into owned storage first.
4. An older value or tombstone can hide the newer update from a higher file
   number, producing the wrong logical result.
5. No. Joining protects lifetime and completion, not sequence selection.
   Every read must also use the same retained sequence.
6. A higher-priority value at 20 and later value at 30, both visible at 40,
   make first-match return 20. Source dominance is a layout/history invariant.
7. A's largest internal key at 80 sorts before the target at 50 under
   descending sequence order; B can contain the next eligible history.
8. Every remaining stream element follows its head. A stale head breaks
   the premise used to choose the global minimum.
9. Count logical streams, moves, seeks/direction changes, and comparison
   bytes, not just files. Asymptotic selection cost alone is insufficient.
10. Nonoverlapping internal-key file ranges support the binary-search
    argument. Overlapping ranges can require visiting several candidates.

</details>

## Source tour

| Entry point | Interface or symbol | Question to answer | Representative evidence |
|---|---|---|---|
| [`database_read.cc`](../../src/engine/database_read.cc) | `Get`, `ReadSources`, `NewIterator` | Which snapshot and source owners are captured under the mutex? | [`database_read_test.cc`](../../tests/unit/engine/database_read_test.cc) |
| [`multi_get.cc`](../../src/api/multi_get.cc) | one-sequence bounded batch reads | Which proof gives a shared view, and which keeps worker state alive? | [`public_multiget_test.cc`](../../tests/unit/api/public_multiget_test.cc), [`multi_get_internal_test.cc`](../../tests/unit/api/multi_get_internal_test.cc) |
| [`lookup.cc`](../../src/engine/lookup.cc) | `LookupValue`, overlapping-file visitation | Why do deletion and missing produce different control flow? | [`lookup_test.cc`](../../tests/unit/engine/lookup_test.cc) |
| [`iterators.cc`](../../src/engine/iterators.cc) | `AppendLevelIterators`, merge and concatenating iterators | Why are L0 and nonzero levels composed differently? | [`iterators_test.cc`](../../tests/unit/engine/iterators_test.cc), [`compaction_test.cc`](../../tests/unit/engine/compaction_test.cc) |
| [`db_iterator.cc`](../../src/engine/db_iterator.cc) | public visibility and direction state | How are internal versions collapsed into one user-key stream? | [`db_iterator_test.cc`](../../tests/unit/engine/db_iterator_test.cc) |
| [`seek_statistics.cc`](../../src/engine/seek_statistics.cc) | sampled seek charges | How can reads request future maintenance without changing their result? | [`seek_statistics_test.cc`](../../tests/unit/engine/seek_statistics_test.cc) |

Follow with [ADR-0030](../adr/0030-point-reads.md),
[ADR-0031](../adr/0031-iterators.md), and the completed baseline in
[ADR-0053](../adr/0053-leveldb-read-path-parity.md).
