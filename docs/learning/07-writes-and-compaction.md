# 07. Writes and Compaction

[Learning path](README.md) | Next: [C++ and concurrency](08-cpp-ownership-errors-and-concurrency.md)

Prerequisites: [durability](04-wal-and-recovery.md) and
[source visibility](06-reads-and-iterators.md).

## One leader, many callers

Multiple application threads can call Write, but one queue leader performs
a commit at a time. Its stack-owned writer remains in the queue until
completion.
Followers wait until they are included in a group or become the next leader.

```text
queue: [leader A] [B] [C] [D]
            |
            +-- choose compatible group A+B+C
            +-- one WAL append, optional sync, memtable insertion
            +-- complete A+B+C, wake D
```

Grouping amortizes log and synchronization work.
It does not add a timer that deliberately waits for future callers.
Only followers already queued during group construction are considered.

### Sync compatibility

| Leader | Follower | Can the follower join, subject to other limits? |
|---|---|---|
| Async | Async | Yes |
| Async | Sync | No |
| Sync | Async | Yes; it receives the stronger grouped durability |
| Sync | Sync | Yes |

An async leader cannot acknowledge a sync follower without providing the
required barrier.

The group-size policy follows LevelDB: normally a 1 MiB grouping bound;
for a leader no larger than 128 KiB, growth is limited to another 128 KiB.
This is a follower-admission policy, not a public maximum batch size:
an already larger leader still commits on its own.

## The commit boundary

The leader makes room, reserves a sequence interval, and commits:

```text
validate owned batch mutation at its boundary
    -> prepare group's starting sequence
    -> release database mutex
    -> WAL append / optional sync / trusted memtable insertion
    -> reacquire mutex
    -> publish final sequence and complete writers
```

Queue-front leadership prevents a different writer from rotating the
borrowed WAL and memtable while this commit is unlocked.
Shared topology changes and publication still require the database mutex.

`Write(const WriteBatch&)` copies its batch, preserving the shared immutable
caller contract. `WriteExclusive(WriteBatch&)` borrows it exclusively to
avoid that copy.
During the exclusive call, no other thread may read, copy, mutate, or submit
that same batch. Its public contents are unchanged when the call finishes.

A group starts with the leader directly and needs reusable scratch only
when a follower is actually included.
The hidden sequence assignment is restored before the caller's batch
returns. These are ownership-aware optimizations, not new data semantics.

## Rotation and flush

A full mutable memtable becomes immutable, and a new WAL/memtable pair
accepts later writes.
There is one ordinary immutable slot; a writer that needs another rotation
waits if the previous immutable is still pending.
Older memtables may remain retained by readers after that slot is cleared.

Background flush builds and syncs a table, protects its output number,
installs the MANIFEST edit, clears the immutable slot, and wakes writers.
It selects L0, L1, or L2 according to overlap and grandparent limits.
Recovery flushes use L0 instead.

The current public facade does not expose Flush.
`DatabaseEngine::FlushMemTable` is an internal deterministic barrier used
by engine callers and tests; it is not a public maintenance API.

## How compaction is selected

There are seven levels, numbered zero through six.
Size compaction scores the eligible source levels:

```text
L0 score = number of files / 4
L1 score = bytes / 10 MiB
L2 score = bytes / 100 MiB
L3 score = bytes / 1000 MiB
...
```

Score at least one means size pressure.
Otherwise an exhausted seek budget can select a file.
Compaction pointers spread size-selected work through a level.

Picking inputs is not simply "choose one file and its neighbor":

- L0 overlaps can expand the source set transitively.
- Next-level files overlapping the input range must be included.
- Boundary files sharing a user key need compatible inclusion.
- Grandparents in the following level constrain overlap and output size.

For a single input with no next-level overlap and acceptable grandparent
overlap, a **trivial move** changes metadata without rewriting its bytes.

## Which versions may be discarded?

Compaction merges inputs in internal-key order.
Let `S` be the oldest registered snapshot, or the current published sequence
if there are none.

For one user key, suppose the entries are:

```text
105 Value
101 Value
 99 Value
 70 Value

oldest snapshot S = 100
```

The algorithm keeps 105, 101, and 99, and can drop 70:
once it has seen an entry at or below 100, that entry hides older versions
from every retained snapshot and every newer reader.

This rule is conservative. It need not retain only the exact minimal set
for the individually registered snapshots.

### Tombstone safety

A deletion at sequence `D` can be discarded only when:

1. `D <= S`, so no retained snapshot needs a pre-deletion state.
2. No lower-level file range can contain an older value for that user key.

Without the second check, removing a tombstone could uncover an old value
outside the compaction inputs.

For the running `color@42/41/40` example with snapshot 40 still registered,
history cannot simply be collapsed to "absent."
After snapshots are released, the deletion may disappear once lower-level
overlap no longer requires it.

## Output installation and reclamation

Compaction splits outputs at the target file size and can split early to
limit overlap with grandparents.
It syncs output tables and their names, then installs one edit replacing
the input files.

Installing a new Version does not immediately free every input:
old Versions, iterators, and pending outputs can still keep files live.
`RemoveObsoleteFiles` constructs a protected live set before deleting
unneeded files outside the database mutex.

After an uncertain background error, further writes, scheduling, and
obsolete-file cleanup are stopped. Reads are not automatically converted
into writes or repair attempts.

## Backpressure is a correctness-of-progress feature

If foreground writes outrun background work, L0 read amplification and
storage debt keep increasing. The engine follows LevelDB's thresholds:

| L0 count | Behavior |
|---|---|
| At least 4 | L0 has a size-compaction score of at least one |
| At least 8 | Each ordinary write can take one 1 ms slowdown |
| At least 12 | A write needing rotation waits for background progress |

The stop check is reached after the "current memtable has room" check.
It is not a blanket rule that every write blocks immediately at twelve
files.

Backpressure does not guarantee a device-throughput or tail-latency target.
It prevents unbounded foreground progress from overwhelming the chosen
single-background-work pipeline.

## Source tour

| File | Focus |
|---|---|
| [`write_path.h`](../../src/engine/write_path.h) | Queue, leader guard, and completion |
| [`write_path.cc`](../../src/engine/write_path.cc) | Group admission and WAL-before-insertion |
| [`database.cc`](../../src/engine/database.cc) | `MakeRoomForWrite`, `FlushImmutable`, and `Compact` |
| [`flush.cc`](../../src/engine/flush.cc) | Initial output-level choice |
| [`compaction_picker.cc`](../../src/engine/compaction_picker.cc) | Scores, overlaps, and trivial moves |
| [`compaction.cc`](../../src/engine/compaction.cc) | Retention rules, output splitting, and edits |
| [`compaction_test.cc`](../../tests/unit/engine/compaction_test.cc) | Snapshot, tombstone, and failure cases |

Read [ADR-0032](../adr/0032-write-path.md),
[ADR-0035](../adr/0035-running-compactions.md), and the completed write
pipeline audit in [ADR-0060](../adr/0060-leveldb-write-path-parity.md).

## Self-check

1. Can an async leader include a sync follower?
2. Why is "this key is deleted" insufficient to drop its tombstone?
3. Why can compaction finish without immediately reducing all disk usage?
4. Are bytes reported as logical benchmark writes a measurement of write
   amplification?

<details>
<summary>Answers</summary>

1. No. That group would not supply the follower's requested sync.
2. Old snapshots or lower-level values can still require the deletion/history.
3. Retained old sources can keep input files alive, and other maintenance
   work may remain outstanding.
4. No. Write amplification also requires physical bytes for the explicitly
   defined WAL/flush/compaction interval and accounting policy.

</details>
