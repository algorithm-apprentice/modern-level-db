# Native Resource Reclamation Validation

## Purpose

Strengthen evidence that Modern LevelDB reclaims obsolete native files and
releases owned resources after their last legitimate pin.

Existing tests cover individual cleanup and RAII boundaries, but a successful
long mixed campaign exposed a missing composition check:

```text
final MANIFEST live SSTables: 3
physical .ldb files:          6
```

The three extras were the inputs deleted by the final installed compaction
edit. The campaign had no surviving Snapshot, Iterator, database process, or
reported error.

This design turns that observation into a native regression before changing
cleanup behavior.

## Existing evidence

The repository already proves:

- failed table builds remove their partial output when possible;
- flush removes the obsolete WAL;
- background compaction removes replaced input tables in MemoryFileSystem
  tests;
- open removes orphan logs, tables, temporary files, and old MANIFESTs;
- uncertain metadata failure suppresses unsafe reclamation;
- pending compaction/flush outputs are protected;
- snapshots, iterators, table-cache handles, mapped views, filesystem handles,
  database locks, executor tasks, and cache pins use explicit ownership tests;
- Windows acquisition failures return to zero live native handles/views;
- POSIX descriptors and locks release through RAII;
- ASan/UBSan, TSan, crash, and process-owner jobs run in hosted CI.

These are necessary but do not prove that a final native compaction leaves a
bounded physical directory without requiring another open or write.

## Reclamation invariants

### Quiescent success

After accepted background work finishes and no Snapshot/Iterator/read/table
handle pins an older Version:

- every physical `.ldb` file is referenced by the current installed Version;
- only the current WAL remains;
- only the current MANIFEST remains;
- no `.dbtmp` file remains;
- no pending output number remains;
- no background task remains scheduled;
- no sticky error exists.

### Legitimate retention

An old file may remain while:

- an Iterator pins a Version or table source;
- a Snapshot requires old sequence history that compaction retained;
- a flush/compaction output number is pending;
- an uncertain metadata error prevents deciding which topology is durable;
- native deletion failed and a later safe cleanup retry is required.

Tests must distinguish these states instead of treating every extra file as a
leak.

### Failure policy

Obsolete-file deletion is reclamation, not publication. A deletion failure
must not roll back or corrupt an installed Version.

The engine must nevertheless:

- preserve the primary database status;
- retain enough state to retry cleanup at the next safe boundary;
- never report an obsolete file as live metadata;
- expose deterministic test evidence that the retry eventually removes it.

## TDD sequence

1. Add a native no-pin compaction test that compares current Version table
   count with physical `.ldb` count after `WaitForBackgroundWork`.
2. Repeat compaction enough times to reproduce final-input leftovers.
3. Confirm the same test on Windows and POSIX.
4. Add a pinned-Iterator control proving old inputs remain while pinned.
5. Release the pin, trigger the documented cleanup boundary, and require the
   physical count to converge to the current live count.
6. Inject one obsolete-file removal failure and require a later retry.
7. Add a native directory-ledger helper that counts WAL, MANIFEST, table, and
   temporary file types without parsing success from file names alone.
8. Run a bounded long mixed campaign and require directory growth to remain
   bounded by live metadata plus documented active resources.

## Candidate root causes

The regression determines which of these applies:

- cleanup runs before the last compaction input/table handle releases;
- table-cache eviction still leaves a native owner that blocks deletion;
- Windows delete sharing is insufficient at one table-open path;
- a removal error is silently discarded and never retried before shutdown;
- the final background task publishes after the last cleanup opportunity.

No fix is selected until the native failing test identifies the boundary.

## Resource-leak validation

File cleanup and process-resource cleanup use separate evidence:

| Resource | Required evidence |
|---|---|
| SST/WAL/MANIFEST/temp names | live-versus-physical native ledger |
| Pending output numbers | `DatabaseState::protected_output_count` |
| Background task | scheduled/queued state reaches zero |
| Snapshot registrations | count reaches zero after release/destruction |
| Table/cache pins | eviction plus owner-destruction tests |
| Native handles/views | platform fault-operation live counters |
| Directory lock | second open succeeds after owner destruction |
| Child processes | owned timeout/crash campaign leaves no survivor |
| Heap allocations | sanitizer jobs plus bounded stress RSS evidence |

Passing one row does not imply the others.

## Local acceptance

- Windows and WSL native regression tests;
- at least 20 compaction cycles with no external pins;
- pinned/unpinned control;
- one injected delete failure followed by retry;
- one 32-thread mixed campaign with repeated reopens;
- directory ledger before close, after close, and after one clean reopen;
- no hosted long-running campaign added.

## Nonclaims

- Immediate deletion is not required while a valid pin exists.
- Native filesystem/device failures can delay reclamation.
- Sanitizers do not prove the absence of disk-space leaks.
- A bounded directory ledger does not prove all OS resources are leak-free.
- The fix must not weaken uncertain-write or old-Version retention rules.

## Delivery sequence

1. Commit this design.
2. Add the failing native live-versus-physical regression.
3. Identify and fix the exact cleanup boundary.
4. Add pinned and injected-removal controls.
5. Add bounded native campaign evidence and documentation.
6. Run Windows/WSL/full CI and deliver one PR.
