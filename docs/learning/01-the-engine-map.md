# 01. The Engine Map

[Learning path](README.md) | Next: [Bytes and formats](02-bytes-and-formats.md)

## The problem the engine solves

The public abstraction is an ordered map from arbitrary byte keys to
arbitrary byte values. It must support updates, ordered iteration, a stable
view of older data, concurrent callers, and persistence.

A plain `std::map` can provide ordering but loses everything when its
process exits. Rewriting a complete sorted file after every update would
persist the map but make small writes increasingly expensive.

An **LSM tree**, short for log-structured merge tree, takes another approach:
append updates to a log, index them in memory, write sorted immutable files
in batches, and merge those files in the background.

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

## Source tour

Read these entry points without following every helper yet:

| File | Focus | Question |
|---|---|---|
| [`db.h`](../../include/modern_leveldb/db.h) | `Database` | What can an application actually do? |
| [`database.h`](../../src/engine/database.h) | Engine members | Which objects are owned per database? |
| [`database.cc`](../../src/engine/database.cc) | `Write`, `Get`, `BackgroundCall` | Where are foreground and background work connected? |
| [`recovery.cc`](../../src/engine/recovery.cc) | `RecoverDatabase` | What is reconstructed before writes start? |
| [`architecture.md`](../architecture.md) | Layer rules | Why is format code independent of filesystem I/O? |

The dependency direction is important: the public facade depends on the
engine, not the other way around. Pure format code cannot schedule
compaction or open a file.

## Self-check

1. Why are both a WAL and a memtable needed?
2. Can a complete table left after a crash always be added to the database?
3. Does moving an entry from L0 to L1 change its sequence number?

<details>
<summary>Answers</summary>

1. The WAL supports recovery; the memtable supports ordered, efficient
   in-memory reads. Neither replaces the other's job.
2. No. It may be an uninstalled output. The recovered metadata determines
   which tables are live.
3. No. Compaction preserves retained entries' logical sequence numbers.

</details>
