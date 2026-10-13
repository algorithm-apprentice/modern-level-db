# Native Resource Reclamation Validation

## Purpose

Strengthen evidence that Modern LevelDB matches LevelDB's deferred obsolete-file
reclamation while releasing native process resources at their ownership
boundaries.

Existing tests cover individual cleanup and RAII boundaries, but a successful
long mixed campaign exposed a missing composition check:

```text
final MANIFEST live SSTables: 3
physical .ldb files:          6
```

The three extras were inputs deleted by the final installed compaction edit.
This is legal deferred disk reclamation when the compaction cleanup observed
an Iterator-pinned old Version and no later cleanup boundary ran.

The observation becomes a characterization test, not a reason to add behavior
that differs from LevelDB.

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

These are necessary but do not imply that closing an otherwise healthy DB runs
another obsolete-file GC pass.

## Reclamation invariants

### Cleanup-boundary success

After `RemoveObsoleteFiles` runs at open/recovery, flush, or compaction and no
Snapshot/Iterator/read/table handle pins an older Version:

- every physical `.ldb` file is referenced by the current installed Version;
- only the current WAL remains;
- only the current MANIFEST remains;
- no `.dbtmp` file remains;
- no pending output number remains;
- no background task remains scheduled;
- no sticky error exists.

### Close semantics

Close/destruction must release:

- WAL/file/mapping handles;
- the native directory lock;
- background workers;
- cache and iterator owners;
- heap allocations.

Close is not an additional obsolete-file GC boundary. An obsolete filename may
remain after close and must be reconsidered by the next open/recovery,
flush, or compaction.

### Legitimate retention

An old file may remain while:

- an Iterator pins a Version or table source;
- a Snapshot requires old sequence history that compaction retained;
- a flush/compaction output number is pending;
- an uncertain metadata error prevents deciding which topology is durable;
- native deletion failed and a later safe cleanup retry is required.
- a pin was released after the final cleanup boundary and the DB then closed.

Tests must distinguish these states instead of treating every extra file as a
leak.

### Failure policy

Obsolete-file deletion is reclamation, not publication. A deletion failure
must not roll back or corrupt an installed Version.

The engine must:

- preserve the primary database status;
- retain enough durable metadata for the next safe boundary to rediscover and
  retry cleanup;
- never report an obsolete file as live metadata;
- expose deterministic test evidence that the retry eventually removes it.

## TDD sequence

1. Add a no-pin compaction control that converges immediately.
2. Pin the pre-compaction Version with an Iterator.
3. Prove the compaction cleanup does not attempt to remove pinned inputs.
4. Release the Iterator and close; require the old filenames to remain legal.
5. Reopen and require the physical table set to converge to live metadata.
6. Inject one obsolete-file removal failure and require open to retry it.
7. Confirm Windows and POSIX handle/lock RAII independently.
8. Run a bounded mixed campaign and compare the ledger before and after reopen.

## Confirmed root cause

`NewInternalIterator` retains a `shared_ptr<const Version>`. `VersionSet` keeps
weak references to installed Versions, and `LiveFiles()` includes every Version
whose shared owner is still live.

Therefore:

```text
Iterator retains old Version
→ compaction installs new Version
→ RemoveObsoleteFiles sees old inputs as live and does not call RemoveFile
→ Iterator later releases the old Version
→ no later cleanup event occurs before close
→ obsolete filenames remain until the next cleanup boundary
```

This matches LevelDB's reference-release and deferred-GC design. It is not a
native handle leak or failed deletion.

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
- one injected delete failure followed by retry on open;
- one 32-thread mixed campaign with repeated reopens;
- directory ledger after close and after one clean reopen;
- no hosted long-running campaign added.

## Nonclaims

- Immediate deletion is not required while a valid pin exists.
- Close does not guarantee that obsolete filenames have already been removed.
- Native filesystem/device failures can delay reclamation.
- Sanitizers do not prove the absence of disk-space leaks.
- A bounded directory ledger does not prove all OS resources are leak-free.
- Tests do not change the production reclamation trigger set.

## Delivery sequence

1. Commit this design.
2. Add live-versus-physical characterization.
3. Confirm the exact Iterator/Version pin chain.
4. Add no-pin, deferred-open, and injected-removal controls.
5. Add bounded native campaign evidence and documentation.
6. Run Windows/WSL/full CI and deliver the tests/documentation without a
   production behavior divergence.
