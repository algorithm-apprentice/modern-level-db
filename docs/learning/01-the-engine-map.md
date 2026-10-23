# 01. The Engine Map

[Learning path](README.md) | Next: [Bytes and formats](02-bytes-and-formats.md)

## Lesson map

| Question | Answer |
|---|---|
| What problem is this lesson about? | Keeping an ordered key/value map readable, durable, and maintainable without rewriting all data after every update |
| Who consumes the engine? | Applications through the public `Database` API |
| What interface matters first? | Put, Get, Delete, WriteBatch, Snapshot, Iterator, and database open/close behavior |
| What implementation will we open? | WAL, memtables, SSTables, Version metadata, and background work |
| What should you be able to do afterward? | Trace one key through write, read, flush/compaction, and recovery at a high level |

## The problem the engine solves

The public abstraction is an ordered map from arbitrary byte keys to
arbitrary byte values. It must support updates, ordered iteration, a stable
view of older data, concurrent callers, and persistence.

### Define the logical object before choosing a layout

Model the history as records `(key, sequence, kind, value)`, with sequence
numbers ordering operations in the represented history. For a key x and
read boundary S, select the record with the largest sequence at most S.
A selected deletion or no
selected record means absence; a selected value can itself be empty.
This is not a list of caller acknowledgements: failed writes can have
uncertain durable outcomes, as [lesson 04](04-wal-and-recovery.md) explains.

This definition describes behavior, not files. It gives a test for every
representation change:

```text
for every key x and every retained read boundary S:
    visible(before, x, S) = visible(after, x, S)
```

Ordered iteration is the visible nondeleted keys in comparator order.
An atomic batch publishes one completed boundary for all its operations;
it does not make independently timed reads into a transaction.
Durable recovery adds a separate condition: acknowledged synchronized
updates must remain represented under the stated failure model.

### Derive the log/index split

A plain `std::map` can provide ordering but loses everything when its
process exits. Rewriting a complete sorted file after every update would
persist the map but make small writes increasingly expensive.

An append-only history avoids those rewrites, but a lookup may have to
inspect the history to identify its newest visible record. An in-memory
ordered index makes that search cheap while the process lives. Neither
alone solves both recovery and ordered lookup.

An **LSM tree**, short for log-structured merge tree, takes another approach:
append updates to a log, index them in memory, write sorted immutable files
in batches, and merge those files in the background.

Each step addresses a different obligation: the log records a replayable
history, memory indexes it, sorted runs bound persistent lookup work, and
merging prevents an unlimited number of runs. This does not eliminate
maintenance; it moves and batches it.

```text
Application
    |
    v
Public RAII API
    |
    v
Database engine
    |
    +-- write ----------> WAL --> mutable memtable
    |
    +-- read -----------> memtables --> version --> table cache --> SSTables
    |
    +-- background -----> immutable flush / compaction
    |
    +-- metadata -------> MANIFEST and CURRENT
```

The WAL provides a replay path. The memtable provides an ordered memory
index. SSTables provide the long-lived sorted representation. Metadata says
which of those files belong to the database's current state.

## The important objects

| Object | What it represents | Why it exists |
|---|---|---|
| Write batch | Ordered Put/Delete operations | Persist and publish a group atomically |
| Mutable memtable | The current in-memory update index | Make new data readable without rewriting tables |
| Immutable memtable | A former mutable memtable | Let background flush proceed while new writes continue |
| SSTable | An immutable sorted key/value file | Support efficient persistent reads and sequential merges |
| Version | A topology of live tables across levels | Let readers retain a stable set of files |
| Snapshot | A registered visible sequence number | Preserve the logical history that a reader needs |
| MANIFEST | A log of metadata edits | Recover the live table topology and counters |
| CURRENT | The name of the selected MANIFEST | Select the authoritative metadata log |

**A Version and a Snapshot are not the same thing.** A Version describes
files; a Snapshot describes which operation sequences are visible.

## Four paths worth learning first

### Write

```text
queue -> make room -> assign sequences -> append WAL
      -> optional sync -> insert into memtable -> publish last sequence
```

The queue has one leader at a time. Several callers may be committed in one
group. A reader does not see half a successful batch because the engine
publishes its final sequence only after all entries are inserted.

### Read

```text
choose visible sequence -> mutable -> immutable
                        -> overlapping L0 tables, newest first
                        -> a candidate in each deeper level
```

A deletion is an answer, not permission to continue searching for an older
value. Otherwise deleting a key could reveal it again.

### Flush and compaction

A flush writes an immutable memtable to an SSTable and installs a metadata
edit. A compaction merges selected tables, removes history that is safe to
discard, and installs replacement files.

The simple teaching picture is "flush into L0, then compact downward."
The actual normal flush policy may place a table directly in L1 or L2 when
overlap permits. Recovery writes replayed memtables into L0.

### Recovery

```text
lock directory -> read CURRENT -> replay MANIFEST
               -> find relevant WALs -> replay complete batches
               -> install recovered tables and a new WAL
```

Recovery does not infer live data by treating every `.ldb` file in a
directory as authoritative. A crash may leave complete but uninstalled
files behind.

## Follow one key

Consider the running example:

```text
color@42 Deletion
color@41 Value "blue"
color@40 Value "red"
```

These are three internal entries, not three user keys.
At sequence 42, the deletion wins. At sequence 40, the newer entries are
invisible, so `"red"` wins.

A flush can move those entries from memory to a table. A compaction can move
them between tables. The answer must stay the same. Physical maintenance
changes representation, not user-visible history.

## Why maintenance needs three proofs

The visible-value equation is the **logical proof**. It is insufficient on
its own. A reader already using old storage needs a **physical lifetime
proof**, and recovery needs a **durable installation proof**.

For example, replacing two sorted runs with one equivalent run can still
fail if old files are deleted under a live reader, or if metadata durably
references an output whose name was never made durable.

```text
logical equivalence: same result for every protected view
physical retention: old borrowers remain valid until they finish
durable installation: recovery never selects an incomplete representation
```

This decomposition is reusable beyond LSM trees: immutable indexes, versioned
configurations, and copy-on-write structures need analogous arguments.

## The cost tradeoff

LSM trees trade inexpensive append-oriented foreground writes for later
maintenance work:

| Cost | Meaning | Main mechanism affecting it |
|---|---|---|
| Write amplification | Physical bytes written per logical update byte | WAL, flush, and repeated compaction |
| Read amplification | Extra sources or blocks examined for a read | Level topology, filters, and caches |
| Space amplification | Physical storage beyond current live data | Old versions, tombstones, and pending compaction |

Reducing one cost can increase another. Larger files, for example, can reduce
file-management work but make individual compactions larger.
No single knob makes every workload better.

### Count the chosen interval, including its omitted work

Assume eight fixed-size live records and four updates, each replacing an
existing key. Count only foreground record writes in these two idealized
designs:

<!-- foreground-cost-example -->

| Mechanism | Updates | Foreground records per update | Foreground record writes |
|---|---|---|---|
| Rewrite sorted file | 4 | 8 | 32 |
| Append update log | 4 | 1 | 4 |

The arithmetic explains the attraction of appending. It is **not** a claim
that total LSM writes are four: a later flush writes data again, compaction
rewrites runs, metadata has a cost, and snapshots can retain old versions.
The example also omits checksums, variable record sizes, and real device
behavior. Choosing a timed boundary is part of a cost claim.

## Exercises: specify, falsify, and transfer

1. Why are both a WAL and a memtable needed?
2. Can a complete table left after a crash always be added to the database?
3. Does moving an entry from L0 to L1 change its sequence number?
4. What incorrect result could occur if a read searched old SSTables before
   the mutable memtable?
5. **Counterexample:** replace the three-version `color` history with only
   its newest record. Which registered view becomes impossible to answer?
6. **Cost:** reproduce the foreground table, then add one flush of the four
   appended records. Why is that still not a complete write-amplification
   measurement?
7. **Design:** propose a log-only and a sorted-file-only engine. For each,
   identify an operation whose cost or guarantee is missing.
8. **Transfer:** a copy-on-write index preserves all lookup answers but frees
   its old nodes immediately. Which of the three maintenance proofs fails?

<details>
<summary>Answers</summary>

1. The WAL supports recovery; the memtable supports ordered, efficient
   in-memory reads. Neither replaces the other's job.
2. No. It may be an uninstalled output. The recovered metadata determines
   which tables are live.
3. No. Compaction preserves retained entries' logical sequence numbers.
4. An older table value or deletion could win before the read reaches the
   newer in-memory update.
5. Snapshot 40 needs `"red"` even when the newest record is the deletion.
   Current-state equality alone does not preserve retained history.
6. The example becomes four foreground plus four flush record writes, but
   outstanding compaction, the WAL/data byte ratio, and metadata remain
   outside that count.
7. Log-only lookup can inspect long history; sorted-file-only updates can
   rewrite the map. A complete answer supplies an index/recovery mechanism
   or explicitly accepts the missing cost/guarantee.
8. Physical retention fails: equal abstract answers do not make a borrowed
   pointer valid after reclamation.

</details>

## Source tour

Read these entry points without following every helper yet:

| Entry point | Interface or symbol | Question to answer | Representative evidence |
|---|---|---|---|
| [`db.h`](../../include/modern_leveldb/db.h) | `Database` | What can an application ask the engine to do? | [`public_api_test.cc`](../../tests/unit/api/public_api_test.cc) |
| [`database.h`](../../src/engine/database.h) | `DatabaseEngine` members | Which owners and synchronization domains exist per database? | [`database_test_support.h`](../../tests/unit/engine/database_test_support.h) |
| [`database.cc`](../../src/engine/database.cc) | `Open`, construction, recovery installation | Which stable resources and recovered owners form one database? | [`database_lifecycle_test.cc`](../../tests/unit/engine/database_lifecycle_test.cc) |
| [`database_write.cc`](../../src/engine/database_write.cc) | `Write`, rotation, commit | Where does foreground mutation cross durability and visibility boundaries? | [`database_write_test.cc`](../../tests/unit/engine/database_write_test.cc) |
| [`database_read.cc`](../../src/engine/database_read.cc) | `Get`, iterator creation, snapshots | Which read view is captured under the database mutex? | [`database_model_test.cc`](../../tests/model/database_model_test.cc) |
| [`database_background.cc`](../../src/engine/database_background.cc) | `BackgroundCall`, flush, compaction | Where does foreground pressure become background maintenance? | [`database_background_test.cc`](../../tests/unit/engine/database_background_test.cc) |
| [`recovery.cc`](../../src/engine/recovery.cc) | `RecoverDatabase` | What must be reconstructed before writes start? | [`recovery_test.cc`](../../tests/unit/engine/recovery_test.cc) |

The dependency direction is important: the public facade depends on the
engine, not the other way around. Pure format code cannot schedule
compaction or open a file. Use the current
[architecture](../architecture.md) for the complete layer rules.
