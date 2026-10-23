# 12. Native Filesystem Lifecycles

[Learning path](README.md)

Prerequisites: [WAL and recovery](04-wal-and-recovery.md),
[tables, filters, and caches](05-tables-filters-and-caches.md), and
[C++ ownership, errors, and concurrency](08-cpp-ownership-errors-and-concurrency.md).

> **Advanced maintainer lesson:** the first pass needs the platform guarantees
> in the [reference](../reference/platform-support-and-durability.md), not every
> native acquisition and teardown step below.

## Lesson map

| Question | Answer |
|---|---|
| What problem is this lesson about? | Keeping native resources, caller buffers, namespace operations, and database locks valid across success, failure, and teardown |
| Who consumes this layer? | WAL and MANIFEST I/O, table readers, recovery, cleanup, VersionSet, and database opening |
| What interface matters first? | `SequentialFile`, `RandomAccessFile`, `WritableFile`, `FileLock`, and `FileSystem` |
| What implementation will we open? | POSIX descriptors/mappings/record locks and Windows handles/OVERLAPPED reads/mapped views |
| What should you be able to do afterward? | Draw the owner and terminal event for one native operation and explain which error wins when cleanup also fails |

## Start with owners, not system calls

The portable interface deliberately exposes the same storage operations on
both platforms. The native resources satisfying one operation are different:

| Object or operation | POSIX owner | Windows owner |
|---|---|---|
| Sequential reader | Reader object owns one descriptor | Reader object owns one file handle |
| Copied random reader | Reader object owns one descriptor; `pread` avoids shared position | Reader object owns one overlapped-capable file handle; each call owns an event and `OVERLAPPED` |
| Mapped random reader | Region owns the mapping and limiter slot; the opening descriptor closes after `mmap` succeeds | View owns the mapping and limiter slot; opening file and section handles close after view acquisition |
| Writable file | Writer owns one descriptor and its buffered first-error state | Writer owns one handle and its buffered first-error state |
| Database lock | Lock object owns a descriptor plus process-local reservation | Lock object owns the handle carrying the kernel lock |

Each returned file or lock owns what it needs independently of the
`FileSystem` object that created it. Tests destroy the filesystem, unlink or
rename the path where the platform permits it, and continue using the retained
reader. A pathname helps describe errors; it is not the resource keeping bytes
alive.

For every path below, ask:

```text
What resource was acquired?
Which object owns it now?
What still borrows it?
What event proves the native operation is terminal?
Which cleanup remains if the next acquisition fails?
```

### Derive a lifecycle proof

Model each acquisition as an obligation attached to one owner. Borrowers
may use the resource only until their documented terminal event; moving an
owner transfers the obligation rather than satisfying it.

For a pending operation, use four states:

```text
not acquired -> owned/pending -> owned/terminal -> released
                    ^                  |
                    |                  +-> report operation result
                    +-- cancellation request is not yet completion
```

The safety invariant is that every live native borrow has a live owner.
Check it initially, then for acquisition, transfer, completion, and release.
An acquisition failure must unwind the successful prefix in a valid order.
Release is justified by terminal completion, not by the decision to return
an error. Conversely, this safety proof does not show that a stalled kernel
operation will eventually complete: progress is a separate assumption.

The state model also explains why one universal "close everything" callback
is insufficient. A file handle, pending request, event, mapping, and limiter
slot have different borrowers and release preconditions.

## Copied reads

### POSIX positioned reads

The copied POSIX random reader owns a `ScopedDescriptor`.
Each call:

1. rejects an offset that does not fit `off_t`;
2. caps the request to `SSIZE_MAX`;
3. calls `pread`, retrying only `EINTR`;
4. returns one successful short-read count, including zero at EOF.

It does not loop to fill the caller's buffer and does not mutate a shared file
position. Concurrent calls can therefore use one reader while the reader and
every output buffer remain alive.

Closing a POSIX descriptor after `close` reports `EINTR` is not retried. The
descriptor may already have been consumed and reused, so a retry could close
an unrelated resource.

### Windows positioned reads

The Windows copied reader opens its file with `FILE_FLAG_OVERLAPPED`, but the
public operation is still synchronous: `Read` does not return until the native
request is terminal.

One call owns:

```text
reader object -> file HANDLE
Read stack    -> OVERLAPPED with explicit Offset/OffsetHigh
event owner   -> manual-reset event referenced by OVERLAPPED
caller        -> output buffer borrowed until Read returns
```

The state machine is:

```text
validate signed 64-bit range
    -> create event
    -> initialize stack OVERLAPPED
    -> ReadFile
         immediate success ----\
         ERROR_IO_PENDING -------+-> GetOverlappedResult(wait = true)
         ERROR_HANDLE_EOF -------> return zero
         other error ------------> return typed error

completion success -> return byte count
completion failure -> capture GetLastError
                      cancel and drain if still pending
                      return EOF or typed error
destroy event only after terminal completion
```

The drain is a lifetime rule, not a retry policy. The kernel must no longer
reference the stack `OVERLAPPED`, event, or caller buffer before any of them
can disappear. Even an error returned from the completion wait is not
permission to return while the operation is still pending.

Each call has a separate event and operation record, so concurrent positioned
reads do not share mutable per-request state.

Consider the tempting failure trace:

```text
completion wait reports an error
request is still pending
caller requests cancellation, destroys event, and returns
kernel later accesses the operation record or output buffer
```

Cancellation requested did not establish terminal completion. The correct
trace preserves all owners while draining, then returns the captured primary
error after the kernel's borrow ends. This is why error handling and lifetime
handling cannot be proved as separate, unrelated functions.

## Mapped-read admission and rollback

Mapping is an optimization selected only when an exact immutable file is
eligible. Both backends choose copied reads for:

- disabled mapping;
- no expected size, an empty file, or an unrepresentable size;
- a size mismatch;
- an exhausted process-wide mapping-count slot.

After a slot is acquired, the native failure policy differs:

| Stage | POSIX | Windows |
|---|---|---|
| Size inspection cannot establish an exact match | Fall back to copied `pread` | Size-query failure is a typed error; a mismatch falls back |
| Native mapping creation fails | Release the pending slot and fall back | Release all partial resources and return the captured typed error |
| Mapping succeeds | Opening descriptor closes; region owns bytes and slot | Section and file handles close; view owns bytes and slot |

The pending mapping owner is rollback:

```text
acquire limiter slot
    -> construct pending region/view owner
    -> attempt native mapping
       copied fallback: pending owner releases the slot;
                        opening handle transfers to the copied reader
       typed failure: pending owners release partial native resources and slot
       success: move pending owner into returned RandomAccessFile
```

This is why the limiter is a shared owner rather than a reference to the
filesystem. A mapped file can outlive the filesystem that admitted it and
still return the slot to the correct limiter.

### Teardown order is a correctness rule

```text
POSIX:   munmap best effort -> release mapping slot
Windows: UnmapViewOfFile must succeed -> release mapping slot
```

The current POSIX destructor cannot report `munmap` failure and treats it as
best-effort cleanup. The current Windows owner treats failure to unmap its
known valid view as an invariant failure and terminates before recycling the
slot. Releasing the slot first would let another mapping consume capacity
while the old view might still exist.

Do not generalize one backend's destructor policy into a portable guarantee.
On the ordinary successful path, both remove the view before releasing its
slot. Their exceptional policies differ: POSIX may release after an unreported
`munmap` failure, while Windows terminates rather than advertise that outcome
as successful teardown.

Mapped bytes remain borrowed from the file view. A table, iterator, or block
using those bytes must retain the owning random-access file through the table
and cache-pin lifetimes described in [lesson 05](05-tables-filters-and-caches.md).
A later page fault can still terminate the process. Applications requiring
typed read errors select copied reads.

## Buffered writers and error priority

Both native writers use a 64 KiB buffer and retain their first I/O error.
`Flush` moves buffered bytes to the operating system; `Sync` first flushes and
then requests the platform's file-content barrier.

A terminal `Close` follows the same composition rule on both backends:

```text
attempt buffer flush
attempt descriptor/handle close even if flush failed
return the earlier flush/sticky error when present
otherwise return the close error
```

Cleanup is still attempted because leaking the native resource does not make
the earlier write failure safer. The cleanup error does not replace the first
causal error because callers need the operation that first made the file
uncertain.

For ordinary reportable errors, define a left-biased composition:

```text
choose(primary, cleanup) = primary if primary is an error
                          cleanup otherwise
```

Enumerating success/failure of both operands proves that a successful cleanup
cannot erase the primary error, and a cleanup error is visible when no earlier
stage failed. The operator is not commutative: swapping stages can change the
diagnostic. It does not authorize unsafe cleanup or override an invariant-
failure policy such as Windows' failed-unmap termination.

POSIX retries interrupted `read`, `pread`, `write`, and sync operations where
the syscall contract permits it. Linux uses `fdatasync` for file data and
`fsync` for directory barriers; macOS attempts `F_FULLFSYNC` for file data and
falls back only for unsupported-operation errors. Windows uses
`FlushFileBuffers`.

Destructors perform only best-effort cleanup. They cannot report that buffered
bytes failed or that a close failed, so durability-sensitive callers use the
explicit `Sync` and `Close` path.

## Directory synchronization preserves a higher-level order

POSIX `SyncDirectory`:

```text
open directory descriptor
    -> fsync directory
    -> close descriptor even if fsync failed
    -> return sync error before close error
```

This is the namespace barrier used before a durable MANIFEST first references
a new file and after `CURRENT` replacement.

Windows has two explicit modes:

- strict mode returns `NotSupported`;
- admitted weak mode opens, validates, and closes the directory but performs
  no POSIX-equivalent persistence barrier.

Higher layers keep the same call order in weak mode. The no-op persistence
policy is therefore visible and consented to; it is not hidden by skipping the
operation or pretending that `FlushFileBuffers` on a data file synced its
directory entry.

## Database locks reflect native ownership

Traditional POSIX `fcntl` record locks are process-associated. Closing another
descriptor for the same lock file can disturb the process's existing lock.
The backend therefore reserves a weakly canonical path identity before opening
another descriptor:

```text
reserve process-local identity
    -> open LOCK descriptor
    -> acquire nonblocking kernel write lock
    -> transfer descriptor and identity into FileLock

failure -> descriptor cleanup + reservation rollback
destruction -> unlock + close + process-local reservation release
```

The reservation rejects same-process aliases before a duplicate open/close can
damage the original kernel lock.

Windows `LockFileEx` locks are associated with the successful handle. The lock
object owns that handle; another failed handle can close without releasing the
first lock. The kernel recognizes case and junction aliases through the
underlying file identity, so Windows does not copy the POSIX process registry.

Both locks are nonblocking and report contention as `Busy`. Process exit
releases their native resources, but that does not make concurrent external
modification of database files supported.

## Capture native errors before cleanup

`errno` and `GetLastError` are thread-local snapshots of the last native
failure, not durable error objects. Another syscall, cleanup operation, or
message-formatting helper can overwrite them.

The backend therefore follows this order:

```text
native call fails
    -> capture errno/GetLastError immediately
    -> perform required rollback or close
    -> format and classify the captured primary error
```

Examples include closing a descriptor after `fcntl` setup fails, draining a
failed overlapped read, and unmapping a view after a later mapping-acquisition
step fails. Error priority is part of the lifecycle: cleanup runs, but it
cannot silently replace the already captured cause unless the earlier stages
succeeded.

## Cost model: count the resource actually budgeted

Let M be the mapping-slot limit and v the active admitted mappings. On
ordinary successful teardown, admission and rollback maintain `0 <= v <= M`:
acquire increases v only below M, and release follows view teardown.
The exceptional POSIX `munmap` policy above prevents extending this to an
unconditional bound on all still-existing kernel mappings after errors.

This is a count budget, not a byte or resident-memory budget. Two slots can
admit two large files; one tiny and one large view consume the same slot
count. Pins can retain either view after a cache mapping is erased.

Copied reads add native-call and copying work; a Windows request also creates
and owns per-call completion state. Mapped reads can avoid that copy but
still incur faults, storage access, and address-space costs. Without file
sizes, residency, request distribution, concurrency, and fault/error policy,
the count model cannot conclude which backend is faster or uses less memory.

## Exercises: prove terminal events and error priority

1. Why must a failed Windows completion wait cancel and drain an operation
   before returning?
2. Why does a mapped reader retain a shared limiter owner instead of borrowing
   its creating filesystem?
3. Why is "release mapping slot, then unmap" the wrong destructor order?
4. Why does POSIX need a process-local lock reservation while Windows does not?
5. If flushing buffered bytes and closing the native writer both fail, which
   error is returned and why is close still attempted?
6. Does successful Windows weak-mode `SyncDirectory` prove a renamed
   `CURRENT` entry survives power loss?
7. Why can copied reads provide typed I/O errors that mapped reads cannot
   promise for every later byte access?
8. **Trace:** after a failed completion wait, cancellation returns but the
   request remains pending. Which resources must survive, and what event
   permits returning?
9. **Calculate:** with M=2 and two retained mapped files, can a third file
   obtain a slot after its old cache mapping is erased but a pin remains?
   Does the answer impose a byte-memory bound?
10. **Counterexample:** a failing native operation sets an error code, then
    successful cleanup changes the thread-local error state. Show how
    formatting afterward can report the wrong cause.
11. **Derive:** enumerate the four primary/cleanup success/failure pairs
    for the left-biased rule. Why is using the last error a different contract?
12. **Transfer:** design a lifecycle for an asynchronous network receive.
    State the terminal event and buffer-borrow rule without assuming that
    closing or cancelling a handle immediately ends native access.

<details>
<summary>Answers</summary>

1. Until terminal completion, the kernel may still reference the stack
   `OVERLAPPED`, its event, and the caller's output buffer.
2. The returned file can outlive the filesystem and must still return its
   reservation to the same process-wide or injected limiter.
3. It can advertise capacity while the old view still exists. Teardown removes
   the owned view before making its count slot reusable.
4. POSIX record locks are process-associated, and opening then closing a
   duplicate descriptor can disturb an existing lock. Windows locks are tied
   to the successful handle.
5. The earlier flush or retained sticky error wins because it first made the
   file uncertain. Close is still attempted to release the native resource.
6. No. Weak mode validates and preserves ordering but supplies no
   POSIX-equivalent directory persistence barrier.
7. A mapped byte can fault long after open or `Read` while an iterator or
   decoder touches the borrowed view. Copied I/O performs the native access
   inside the operation that returns the typed result.
8. The reader handle, operation record, event, and output buffer must remain
   valid until native completion ends the borrow. Cancellation acceptance
   alone is not the terminal event.
9. No slot is free while both views remain retained. This says nothing
   about their lengths, resident pages, or the copied fallback's memory.
10. Capture the first error before rollback. Formatting from the live native
    error state instead can diagnose the cleanup or lose the failure.
11. Success/success gives success; error/success preserves primary;
    success/error reports cleanup; error/error preserves primary.
    A last-error rule instead replaces the causal failure in the fourth case.
12. A valid answer models issue, pending, cancellation, terminal completion,
    and reclamation, including errors while waiting. It distinguishes safety
    from the extra assumptions required for eventual completion.

</details>

## Source tour

| Entry point | Interface or symbol | Question to answer | Representative evidence |
|---|---|---|---|
| [`file_system.h`](../../src/platform/file_system.h) | open-file interfaces and `FileSystem` | Which concurrency, ownership, short-read, and durability obligations are portable? | [`file_system_test.cc`](../../tests/unit/platform/file_system_test.cc) |
| [`posix_file_system.cc`](../../src/platform/posix_file_system.cc) | `ScopedDescriptor`, copied/mapped readers, `SyncAndCloseDirectory`, `LockFile` | Which descriptor or mapping owns each borrow, and why is the process-local lock reservation acquired before open? | [`posix_file_system_test.cc`](../../tests/unit/platform/posix_file_system_test.cc) |
| [`windows_file_system.cc`](../../src/platform/windows_file_system.cc) | `RetirePendingRead`, `WindowsRandomAccessFile`, `WindowsMappedView`, `CreateMappedRandomFile`, `LockFile` | What must reach terminal completion before event, buffer, view, handle, or mapping slot teardown? | [`windows_file_system_test.cc`](../../tests/unit/platform/windows_file_system_test.cc) |
| [`mapped_read_limiter.h`](../../src/platform/mapped_read_limiter.h) | `Acquire`, `Release` | Why must the returned file retain the limiter independently of its filesystem? | [`mapped_read_limiter_test.cc`](../../tests/unit/platform/mapped_read_limiter_test.cc) |

Start with the maintained
[platform support and durability reference](../reference/platform-support-and-durability.md).
Then read [ADR-0011](../adr/0011-filesystem-contracts.md),
[ADR-0056](../adr/0056-leveldb-table-mmap-parity.md),
[ADR-0064](../adr/0064-windows-filesystem-and-delivery.md), and
[ADR-0068](../adr/0068-windows-mapped-read-parity.md) for decision history.

Useful focused tests include:

- `SequentialReaderOutlivesFilesystemAndUnlinkedPath`;
- `CopiedAndMappedOwnersOutliveTheirOriginatingFilesystem`;
- `DirectorySyncReportsFailureAfterClosingDescriptor`;
- `PreventsDuplicateProcessLocksWithoutDroppingKernelLock`;
- `PendingReadsCompleteBeforeTheirEventIsReleased`;
- `NativeMappingFailuresReportErrorsAndRollbackAllResources`;
- `FailedOwnedViewUnmapDoesNotRecycleAnActiveSlot`;
- `LocksContendAcrossInstancesAndReleaseThroughRaii`.
