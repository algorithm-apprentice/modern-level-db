# 04. WAL and Recovery

[Learning path](README.md) | Next: [Tables, filters, and caches](05-tables-filters-and-caches.md)

Prerequisites: [formats](02-bytes-and-formats.md) and
[sequence visibility](03-memory-and-mvcc.md).

## Visibility is not durability

A value can be readable in memory while its durable representation is
still incomplete. It can also reach durable storage before its caller
receives success.

Keep three questions separate:

| Question | Mechanism |
|---|---|
| Can another read see the update? | Published sequence and readable sources |
| Can the update be replayed after process exit? | Complete WAL bytes outside application buffers |
| Must an acknowledged update survive power loss? | Required file and directory synchronization |

These guarantees assume the supported filesystem and storage honor their
synchronization contracts. They are not protection against arbitrary device
failure or malicious modification.

## Append, Flush, Sync, Close

The filesystem interface distinguishes these operations:

| Operation | Meaning |
|---|---|
| `Append` | Add bytes through the writable-file abstraction |
| `Flush` | Push application-buffered bytes toward the operating system |
| `Sync` | Request durable file contents through the backend |
| `Close` | Release the file handle; not a substitute for durability |
| `SyncDirectory` | Make directory namespace changes durable |

A durable file's contents and a durable name for that file are separate
requirements. Renaming a synced temporary file to `CURRENT` is not the
same as durably recording that rename.

`WriteOptions::sync` controls the WAL data barrier for a write.
`Options::sync_wal_creation` protects WAL creation names by default,
including the initial WAL setup. It does not make every asynchronous write
equivalent to `sync=true`.

## The write-ahead rule

The ordinary successful order is:

```text
append complete batch to WAL
    -> Flush performed by AddRecord
    -> Sync if requested
    -> insert all memtable entries
    -> publish final sequence
    -> return success
```

If the memtable became visible before the log, a reader could observe a
value for which the recovery path has no representation.

If a storage operation fails during commit, the result can be uncertain:
some bytes may already have reached storage.
The engine records a sticky first error rather than continuing later
writes on the assumption that nothing happened.
An error is not a promise that the attempted batch is absent after recovery.

## Logical records and physical fragments

One logical WAL record carries a complete write batch. The file consists
of 32 KiB blocks, and a physical record cannot cross a block boundary:

```text
masked CRC32C: 4 bytes
payload length: 2 bytes
fragment type: 1 byte
payload: length bytes
```

The checksum covers the type byte and payload.
Types are Full, First, Middle, and Last.

For a 40 KiB logical record starting at a block boundary:

```text
block 0: First header (7) + payload (32761) = 32768 bytes
block 1: Last  header (7) + payload (8199)  =  8206 bytes
```

The reader returns a batch only after reconstructing the complete logical
record. It cannot apply the first fragment as a partial batch.

Two compatibility details are worth noticing:

- Fewer than seven bytes remaining in a block are padded with zeroes.
- Exactly seven bytes remaining before a nonempty record produces a
  zero-length First fragment, matching LevelDB's historical writer behavior.

The current fragmenter uses a streaming cursor, not a vector of all
fragments. Its payload views borrow the unchanged batch bytes.
ADR-0060 supersedes the original fragment-vector mechanism in ADR-0013.

## EOF, corruption, and I/O error are different

`WalReader::ReadNext` has several outcomes:

| Outcome | Interpretation |
|---|---|
| Logical-record event | A complete record is available |
| Corruption event | Damaged framing was detected; the caller chooses policy |
| Empty optional | End of file, including an incomplete crash-truncated tail |
| Failed Result | A terminal read/I/O failure |

A checksum mismatch is not simply EOF.
A bad physical length is not a reason to scan arbitrary payload bytes for
another plausible header.

Current database WAL replay skips physical corruption events and tolerates
truncated tails. It rejects intact logical records that are malformed
batches or have invalid sequence ordering. MANIFEST recovery treats
corruption events as fatal because they compromise the authoritative
metadata history.

This distinction describes existing policy, not a general promise to repair
any corrupted database without data loss.

## Metadata installation is another commit

The MANIFEST uses WAL framing, but its payloads are `VersionEdit` records:
file additions/deletions, log numbers, sequence counters, comparator
identity, and compaction pointers.

Installing a flush or compaction requires:

```text
write and sync output tables
    -> ensure their directory names are durable
    -> append and sync the MANIFEST edit
    -> publish the new in-memory Version
    -> old files become eligible for cleanup
```

Modern performs the output-directory barrier before the durable MANIFEST
installation. Pending-output tracking protects those files while the
database mutex is released for slow work.

When selecting a new MANIFEST, `CURRENT` is installed through:

```text
write temporary CURRENT contents -> Sync -> Close
    -> rename to CURRENT -> SyncDirectory
```

The file named by `CURRENT` must already be durable.

## Think through crash points

| Crash point | Safe recovery interpretation |
|---|---|
| Before output tables are complete | Old installed Version remains authoritative |
| Tables are durable, edit is not installed | New tables may be orphan outputs |
| Edit is durable, old tables still exist | New Version is recoverable; old files may be redundant |
| Edit references a table whose name was never durable | Unsafe ordering; recovery could require a missing file |

File cleanup must never run ahead of a certain durable installation.
After an uncertain metadata failure, the engine stops mutation and
obsolete-file cleanup instead of guessing.

## Source tour

| File | Focus |
|---|---|
| [`file_system.h`](../../src/platform/file_system.h) | Explicit durability primitives |
| [`wal_format.cc`](../../src/format/wal_format.cc) | Fragment boundaries and checksums |
| [`wal_io.cc`](../../src/wal/wal_io.cc) | Reassembly, tail handling, and first-error poisoning |
| [`recovery.cc`](../../src/engine/recovery.cc) | Directory locking, WAL selection, and checked replay |
| [`version_set.cc`](../../src/metadata/version_set.cc) | `Write`, `InstallCurrent`, and installation order |
| [`power_loss_test.cc`](../../tests/crash/power_loss_test.cc) | Allowed recovered states at failure boundaries |

Then read [ADR-0011](../adr/0011-filesystem-contracts.md),
[ADR-0029](../adr/0029-database-recovery.md), and
[ADR-0040](../adr/0040-compatibility-and-crash-harness.md).

## Self-check

1. Does `Flush` mean an acknowledged write must survive power loss?
2. Why can a table exist on disk but not belong to the recovered database?
3. Why is an in-flight sync batch allowed to appear after a crash even
   though its caller never received success?

<details>
<summary>Answers</summary>

1. No. File and relevant namespace durability require synchronization.
2. Its installing MANIFEST edit may never have become durable.
3. Durability can precede acknowledgement. Recovery may include the complete
   in-flight batch, but must not manufacture a partially applied batch.

</details>
