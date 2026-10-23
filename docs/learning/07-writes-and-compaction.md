# 07. Writes and Compaction

[Learning path](README.md) | Next: [C++ and concurrency](08-cpp-ownership-errors-and-concurrency.md)

Prerequisites: [durability](04-wal-and-recovery.md) and
[source visibility](06-reads-and-iterators.md).

## Lesson map

| Question | Answer |
|---|---|
| What problem is this lesson about? | Serializing concurrent writes and preventing immutable history from growing without bound |
| Who consumes this path? | Concurrent application writers, background flush, and background compaction |
| What interface matters first? | Database Write, writer queue grouping, memtable rotation, flush, and compaction installation |
| What implementation will we open? | Leader/follower grouping, WAL-before-MemTable commit, backpressure, overlap selection, and retention rules |
| What should you be able to do afterward? | Predict a write group and decide which versions/tombstones a compaction must retain |

## One leader, many callers

There are two linked specifications. Foreground commits must publish whole
ordered batches and report their durability/error result. Background work
must replace selected storage without changing any protected read view.
Safety does not imply progress: both protocols can preserve state while
waiting for I/O or a worker that must eventually run or report failure.

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

In a simple model with m compatible queued writes, total encoded bytes B,
one barrier cost F, and byte-processing cost c*B, separate commits pay
`m*F+c*B`; one group pays `F+c*B` plus preparation. This derivation assumes
that one barrier really covers the complete group. It neither eliminates
publication ordering nor predicts caller tail latency.

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

The specification is stronger than "keep the latest value":

```text
for every user key x and protected read sequence Q >= S:
    visible(original history, x, Q) = visible(compacted history, x, Q)
```

The model assumes the compaction inputs include the required overlapping
history and that the lower-level range predicate conservatively detects
outside older values. Current in-memory sources can still contain newer
updates; compaction is not a global rewrite of the database.

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

### Derive the one-key filtering state

For entries of one user key in decreasing sequence order:

```text
previous_sequence = none
for entry in merged input:
    shadowed = previous_sequence exists and previous_sequence <= S
    deletion_obsolete = entry is a deletion and entry.sequence <= S
                        and no deeper range can hold an older value
    if not shadowed and not deletion_obsolete:
        emit entry
    previous_sequence = entry.sequence
```

Reset this state at every new user key. Track the previous **observed**
entry even if it was dropped.

Once a visible predecessor at or below S has been encountered, every older
entry is hidden from all protected Q. If that predecessor was itself an
obsolete deletion, its absence is safe only because no outside older value
can reappear, and the older input versions are also discarded. These cases
preserve either the deciding retained record or the same absence.
Updating state only for emitted records could drop a tombstone and then
incorrectly keep an older input value.

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

<!-- retention-worked-cases -->

| Oldest sequence S | Deeper older history possible | Input sequence/kind | Retained sequence/kind |
|---|---|---|---|
| 100 | no | `105V,101V,99V,70V` | `105V,101V,99V` |
| 80 | no | `90D,70V` | `90D,70V` |
| 100 | yes | `90D,70V` | `90D` |
| 100 | no | `90D,70V` | `none` |

The second row preserves a pre-deletion snapshot. The third protects against
outside resurrection. The last can remove the deletion and all older input
history. These are logical-history decisions, not permission to free files
still pinned by readers.

The [one-key executable model](examples/retention.py) implements only this
filtering rule. Its independent visibility tests compare every protected
read across bounded histories and include deliberately wrong snapshot/base
assumptions that produce counterexamples. It is not an input picker, file
writer, or proof that arbitrary compaction inputs are complete.

### Analyze merge work and outstanding debt

With I input entries and m sorted streams, linear head selection takes
O(I*m) comparisons in the abstract model; a heap changes that component.
Input decoding, variable-length comparisons, retained output bytes, syncing,
and overlap bookkeeping need separate accounting. Physical write
amplification requires actual bytes over an explicit interval, including
the WAL and any maintenance counted by the claim.

## Output installation and reclamation

Compaction splits outputs at the target file size and can split early to
limit overlap with grandparents.
It syncs output tables and their names, then installs one edit replacing
the input files.

Installing a new Version does not immediately free every input:
old Versions, iterators, and pending outputs can still keep files live.
`RemoveObsoleteFiles` constructs a protected live set before deleting
unneeded files outside the database mutex.

After an uncertain mutation or maintenance failure becomes sticky, further
writes, scheduling, and obsolete-file cleanup are stopped. Reads are not
automatically converted into writes or repair attempts.

Follow cleanup in `database_background.cc` as three responsibilities:
`KeepFile` expresses retention policy, `CollectObsoleteFiles` snapshots pinned
Versions and pending outputs and evicts obsolete tables with the mutex held,
and `RemoveObsoleteFiles` deletes the collected paths with the mutex released.
A listing error returns without unlocking; a successful empty collection still
passes through unlock/relock. Filesystem deletion remains best effort, not a
new durability or error-recovery protocol.

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

For a fluid debt model, let foreground rate be lambda updates/second,
maintenance demand alpha bytes/update, and background service mu bytes/second.
Sustained `alpha*lambda > mu` grows debt. More workers help only if they
increase effective service without exhausting the same CPU, device, or
memory resources. A stop threshold supplies waiting, not a proof that a
failed or stopped background executor can make progress.

## Exercises: preserve history while changing representation

1. Can an async leader include a sync follower?
2. Why is "this key is deleted" insufficient to drop its tombstone?
3. Why can compaction finish without immediately reducing all disk usage?
4. Are bytes reported as logical benchmark writes a measurement of write
   amplification?
5. If WAL append returns an error after writing some bytes, may the next
   writer continue using that WAL?
6. **Derive:** reproduce the retention table and state which protected
   query would fail if the second row dropped its old value.
7. **Counterexample:** remove a deletion at 90 with S = 100 while an
   unselected deeper file contains value 70. Show the resurrected result.
8. **State:** why does the pseudocode update previous_sequence even for a
   dropped deletion? Construct the wrong output if it does not.
9. **Cost:** four compatible commits have barrier cost 10 and total byte
   work 4. Compare separate versus grouped cost, then list omitted latency
   and durability assumptions.
10. **Transfer:** design backpressure for a different background pipeline.
    State its debt unit, arrival/service rates, stop condition, and what
    happens when the worker fails permanently.

<details>
<summary>Answers</summary>

1. No. That group would not supply the follower's requested sync.
2. Old snapshots or lower-level values can still require the deletion/history.
3. Retained old sources can keep input files alive, and other maintenance
   work may remain outstanding.
4. No. Write amplification also requires physical bytes for the explicitly
   defined WAL/flush/compaction interval and accounting policy.
5. No. The commit result is uncertain, so the database owner makes the error
   sticky and rejects later writes.
6. A read at 80 must still see value 70 before deletion 90. Rows three and
   four differ because of possible history outside the inputs.
7. Before dropping it, the visible deletion decides absence. Afterward,
   lookup can reach the deeper value and return 70.
8. Otherwise an empty output after dropping deletion 90 can make value 70
   look unshadowed and retain it. Observed visibility, not output count,
   determines whether older input history is obsolete.
9. Separate cost is 44 units, grouped cost 14 in this model. Queued
   compatibility, one covering barrier, preparation, scheduling, and caller
   waiting are not all represented by that arithmetic.
10. A complete design distinguishes bounded resource safety from liveness.
    Waiting forever is not recovery; permanent failure needs an explicit
    error/closing transition and retained-state policy.

</details>

## Source tour

| Entry point | Interface or symbol | Question to answer | Representative evidence |
|---|---|---|---|
| [`write_path.h`](../../src/engine/write_path.h) | `WriteQueue::Write`, guards, completion | Which writer owns group preparation and who receives its result? | [`write_path_test.cc`](../../tests/unit/engine/write_path_test.cc) |
| [`write_path.cc`](../../src/engine/write_path.cc) | `CommitGroup`, `BuildGroup` | Why must WAL append/sync precede trusted MemTable insertion? | [`write_path_test.cc`](../../tests/unit/engine/write_path_test.cc) |
| [`write_buffer_state.h`](../../src/engine/write_buffer_state.h) | active generation, immutable retention, published bytes | Which WAL and MemTables move together during recovery, rotation, and flush? | [`write_buffer_state_test.cc`](../../tests/unit/engine/write_buffer_state_test.cc) |
| [`database_write.cc`](../../src/engine/database_write.cc) | `MakeRoomForWrite`, rotation, commit | When do foreground writers delay, stop, or publish a completed group? | [`database_write_test.cc`](../../tests/unit/engine/database_write_test.cc) |
| [`database_background.cc`](../../src/engine/database_background.cc) | background call, `ApplyBackgroundEdit`, flush and compaction completion | How is foreground maintenance debt discharged without holding the mutex through I/O? | [`database_background_test.cc`](../../tests/unit/engine/database_background_test.cc) |
| [`flush.cc`](../../src/engine/flush.cc) | `FlushMemTable`, output-level choice | Why can some flush outputs start below L0? | [`flush_test.cc`](../../tests/unit/engine/flush_test.cc) |
| [`compaction_picker.cc`](../../src/engine/compaction_picker.cc) | scores, overlaps, trivial moves | Which files must join one compaction input set? | [`compaction_picker_test.cc`](../../tests/unit/engine/compaction_picker_test.cc) |
| [`compaction.cc`](../../src/engine/compaction.cc) | retention, tombstones, output splitting | Which history can be dropped without changing any retained view? | [`compaction_test.cc`](../../tests/unit/engine/compaction_test.cc) |

Read [ADR-0032](../adr/0032-write-path.md),
[ADR-0035](../adr/0035-running-compactions.md), and the completed write
pipeline audit in [ADR-0060](../adr/0060-leveldb-write-path-parity.md).
