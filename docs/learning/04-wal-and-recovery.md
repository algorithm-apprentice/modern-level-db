# 04. WAL and Recovery

[Learning path](README.md) | Next: [Tables, filters, and caches](05-tables-filters-and-caches.md)

Prerequisites: [formats](02-bytes-and-formats.md) and
[sequence visibility](03-memory-and-mvcc.md).

## Lesson map

| Question | Answer |
|---|---|
| What problem is this lesson about? | Making acknowledged state recoverable while distinguishing visibility, process recovery, and power-loss durability |
| Who consumes this layer? | DatabaseEngine, VersionSet, recovery, and offline diagnostics |
| What interface matters first? | WritableFile Append/Flush/Sync/Close, WalWriter, WalReader events, and recovery results |
| What implementation will we open? | WAL fragmentation/checksums, crash tails, the VersionSet prepare/write/publish state machine, directory sync, and cleanup protection |
| What should you be able to do afterward? | Classify a crash point, predict which data or metadata recovery may accept, and explain why a durable Version is published only after prepared I/O |

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

On the supported POSIX boundary, directory synchronization provides the
namespace barrier used by the strict durability model below. Native Windows
opening requires explicit weak-namespace consent: file-content flushes remain
real, but directory/name persistence cannot claim the POSIX-equivalent
guarantee. See
[platform support and durability](../reference/platform-support-and-durability.md).

## Append, Flush, Sync, Close

The filesystem interface distinguishes these operations:

| Operation | Meaning |
|---|---|
| `Append` | Add bytes through the writable-file abstraction |
| `Flush` | Push application-buffered bytes toward the operating system |
| `Sync` | Request durable file contents through the backend |
| `Close` | Release the file handle; not a substitute for durability |
| `SyncDirectory` | Request durable namespace changes where the backend supports that guarantee |

A durable file's contents and a durable name for that file are separate
requirements. Renaming a synced temporary file to `CURRENT` is not the
same as durably recording that rename.

`WriteOptions::sync` controls the WAL data barrier for a write.
`Options::sync_wal_creation` protects WAL creation names by default,
including the initial WAL setup. It does not make every asynchronous write
equivalent to `sync=true`, and it does not strengthen the admitted Windows
namespace guarantee.

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
[ADR-0060](../adr/0060-leveldb-write-path-parity.md) supersedes the original
fragment-vector mechanism in
[ADR-0013](../adr/0013-wal-record-format.md).

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

Under the strict POSIX durability model, installing a flush or compaction
requires:

```text
write and sync output tables
    -> ensure their directory names are durable
    -> append and sync the MANIFEST edit
    -> publish the new in-memory Version
    -> old files become eligible for cleanup
```

Modern performs the output-directory operation before the durable MANIFEST
installation. On POSIX this is the strict namespace barrier. On native
Windows it preserves the ordering without upgrading the explicitly weak
namespace guarantee. Pending-output tracking protects those files while the
database mutex is released for slow work.

When selecting a new MANIFEST, `CURRENT` is installed through:

```text
write temporary CURRENT contents -> Sync -> Close
    -> rename to CURRENT -> SyncDirectory
```

The selected MANIFEST contents must already be durable. Under the strict
model, its directory name must be durable too. Native Windows retains the
sequence but not an equivalent power-loss claim for the rename.

## The VersionSet prepare, write, publish transaction

`VersionSet::LogAndApply` must preserve two different kinds of state:

- persistent metadata represented by the MANIFEST edit;
- current in-memory topology and retention used by readers and cleanup.

The runtime overload is called with the database mutex held. It deliberately
does not hold that mutex during filesystem I/O:

```text
database mutex held
    validate edit and counter bounds
    build the candidate Version
    capture log counters, compact pointers, next-file number, and last sequence
    encode a current-Version snapshot if a new MANIFEST is needed
    allocate the shared candidate and reserve installed-Version tracking
        |
        v
database mutex released
    append and sync the prepared edit
    if this is a new MANIFEST:
        sync its directory name
        install CURRENT through its synced temporary file
        |
        v
database mutex reacquired
    typed write failure -> retain the first error and publish nothing
    success -> publish captured pointers, log counters, and candidate Version
```

Unlocking does not make the candidate mutable. Every byte and owner needed by
the write was prepared under the mutex. The mutex is reacquired before every
normal return and before a thrown exception leaves `WritePrepared`.

Foreground work can still advance allocation and sequence counters while the
I/O is in progress. The MANIFEST record contains the values captured before
unlocking; publication does not roll newer in-memory `next_file_number_` or
`last_sequence_` values backward. The test
`RuntimeApplyUnlocksIoAndPreservesConcurrentCounters` makes this distinction
observable.

The candidate `shared_ptr` and vector capacity are prepared before I/O because
success changes the recovery authority. Once a MANIFEST edit is durable,
installing its in-memory counterpart must not depend on another allocation
that could throw. The final publication is therefore assignments, moves, and
prepared retention work rather than construction of a second candidate.

A returned filesystem error is sticky because the edit may or may not become
recoverable after a crash. The old in-memory Version stays current, later
edits return the retained error without more I/O, and cleanup cannot treat the
candidate as installed. Reopening resolves whether `CURRENT` and the MANIFEST
select the old or new state. A C++ exception is a different channel:
`WritePrepared` reacquires the mutex and propagates it; it does not translate
that exception into a successful-looking or typed storage result.

### Old Versions remain live for concrete reasons

Publishing a new current Version does not immediately authorize deletion of
every file absent from it:

```text
current_ shared owner        -> keeps the new Version live
ReadPin on the old current   -> moves an owner to read_pinned_versions_
iterator/compaction owner    -> keeps its shared Version live directly
versions_ weak registry      -> observes every installed Version still owned
LiveFiles()                  -> unions files from all observed live Versions
```

`ReadPin` is intentionally lighter than taking another public shared owner.
It increments a mutex-protected pin count. During installation,
`RetainPinnedCurrent` gives a pinned old current Version a real owner before
`current_` moves to the candidate. Releasing the last pin removes that retained
owner once the Version is no longer current. Shared iterator or compaction
owners keep their Versions live independently.

This retention model connects metadata publication to file cleanup: a durable
new Version may be current while older files remain protected by active
readers. Cleanup uses `LiveFiles`, pending outputs, and table-cache eviction
rather than assuming "not in current" means "unused."

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

| Entry point | Interface or symbol | Question to answer | Representative evidence |
|---|---|---|---|
| [`file_system.h`](../../src/platform/file_system.h) | `WritableFile`, `SyncDirectory` | Which operation provides visibility, content durability, or namespace durability? | [`file_system_test.cc`](../../tests/unit/platform/file_system_test.cc) |
| [`wal_format.cc`](../../src/format/wal_format.cc) | `WalFragmenter`, `DecodeWalFragment` | How does a logical record cross block boundaries without a partial commit? | [`wal_format_test.cc`](../../tests/unit/format/wal_format_test.cc) |
| [`wal_io.cc`](../../src/wal/wal_io.cc) | `WalWriter`, `WalReader::ReadNext` | Which outcomes are record, crash tail, corruption, and terminal I/O? | [`wal_io_test.cc`](../../tests/unit/wal/wal_io_test.cc) |
| [`recovery.cc`](../../src/engine/recovery.cc) | `RecoverDatabase`, `LogReplay`, `CreateRecoveryLog` | Which logs reconstruct state, and when does the new WAL become durable? | [`recovery_test.cc`](../../tests/unit/engine/recovery_test.cc) |
| [`version_set.cc`](../../src/metadata/version_set.cc) | `LogAndApply`, `ApplyPrepared`, `WritePrepared`, `PublishPreparedEdit`, `LiveFiles` | What is captured before unlocking, what becomes durable, and which old Versions remain protected? | [`version_set_test.cc`](../../tests/unit/metadata/version_set_test.cc), [`power_loss_test.cc`](../../tests/crash/power_loss_test.cc) |

Then read [ADR-0011](../adr/0011-filesystem-contracts.md),
[ADR-0027](../adr/0027-version-set.md),
[ADR-0029](../adr/0029-database-recovery.md), and
[ADR-0040](../adr/0040-compatibility-and-crash-harness.md).

Within recovery, `CreateRecoveryLog` owns acquisition and the optional
file-sync-before-directory-sync barriers. `RecoverDatabase` retains the larger
commit: reserve the log number after replay, create the writer, apply the
MANIFEST edit, then keep built tables and return the new owners. Failure before
publication leaves RAII cleanup in charge. Runtime rotation is deliberately a
different path; it must preserve its old-WAL close and sticky-error protocol.

## Self-check

1. Does `Flush` mean an acknowledged write must survive power loss?
2. Why can a table exist on disk but not belong to the recovered database?
3. Why is an in-flight sync batch allowed to appear after a crash even
   though its caller never received success?
4. If closing the old WAL throws during rotation, should the engine install
   the replacement log and memtable?
5. Why does VersionSet allocate the candidate owner and reserve tracking
   capacity before releasing the database mutex for MANIFEST I/O?
6. Can cleanup delete every file absent from the newly published current
   Version?

<details>
<summary>Answers</summary>

1. No. File and relevant namespace durability require synchronization.
2. Its installing MANIFEST edit may never have become durable.
3. Durability can precede acknowledgement. Recovery may include the complete
   in-flight batch, but must not manufacture a partially applied batch.
4. No. It records a sticky error and leaves the installed state
   unchanged; otherwise the rotation would publish a partially completed
   transition.
5. After the edit becomes durable, publishing its matching in-memory state
   must not depend on an allocation that can throw. Preparation also freezes
   the exact edit and snapshot bytes before concurrent counters may advance.
6. No. Read pins and shared owners can keep older installed Versions live.
   Cleanup must include their files, pending outputs, and cache ownership.

</details>
