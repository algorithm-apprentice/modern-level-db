# Native Writer, Lock and Close Lifecycle

## Scope

Hardest-first native batch for POSIX and Windows writable files, directory sync
and file locks. Source baseline: `d87f42223e7fd292255848dfe4515300600e4fb4`.
Previously delivered path/open/mapping/enumeration/positioned-read work remains
unchanged.

No public API, buffering, syscall retry, sync durability, first-error, terminal
Close, lock contention, destructor cleanup, benchmark or coverage-policy change.
POSIX and Windows transactions remain separate; similar names do not imply
interchangeable resource/error policies.

## POSIX writer

Add `CheckOpen(operation)` to the writer, preserving exact InvalidArgument text
for Append/Flush/Sync after Close. Append retains sticky-error admission,
buffer copy, flush, small remainder buffering and direct WriteAll. Sync retains
flush before data-only descriptor sync.

Close remains the terminal owner: set close_attempted before exchanging the
descriptor, flush through the retained raw descriptor, always attempt close,
prefer flush error, otherwise latch/return close error. Destructor performs only
best-effort buffered write/close when Close was never attempted.

## Windows writer

Add private `FlushBuffer`, matching the existing Flush body: return retained
error, capture/reset buffer, then RememberFirstError(WriteAll). Append, Flush,
Sync and Close reuse it without adding a close_attempted policy Windows does not
currently have. Close still attempts handle close after flush and returns flush
failure first.

## Native locks

Extract platform-local lock acquisition leaves only where they own syscall/error
classification. POSIX process registry reservation, descriptor acquisition,
fcntl lock, owner construction and reservation commit stay visible in LockFile.
Windows OpenRegularFile, LockFileEx, Busy translation and handle transfer stay
visible. Unlock remains destructor-owned and best effort.

## Characterization

Before production edits, strengthen native cases for repeated post-Close
operations, combined flush/close failures, destructor best effort, exact first
Error copies, lock contention/noncontention errors, handle/descriptor release
and retry after failed acquisition. Run actual POSIX and Windows native suites;
one platform cannot stand in for the other.

Independent design and implementation review precede one non-draft PR. Exact
hosted native identities, all CI gates and reviewed-path equality are required.
This batch is not complete native-platform or production certification.
