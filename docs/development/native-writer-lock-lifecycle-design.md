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

Add `CheckOpen(operation, source_location)` to the writer, preserving exact
InvalidArgument text and the public caller method's source location for
Append/Flush/Sync after Close. This remains the first gate, before sticky-error
admission. Append retains sticky-error admission,
buffer copy, flush, small remainder buffering and direct WriteAll. Sync retains
flush before data-only descriptor sync.

Close remains the terminal owner: set close_attempted before exchanging the
descriptor, flush through the retained raw descriptor, always attempt close,
prefer flush error, otherwise latch/return close error. Destructor performs only
best-effort buffered write/close when Close was never attempted.

After a sticky I/O failure and Close, Append/Flush/Sync return their
operation-specific closed InvalidArgument, while repeated Close returns the
original sticky Error. Characterization must compare code, complete message,
source location and independent Error ownership.

## Windows writer

Add private `FlushBuffer`, matching the existing Flush body: return retained
error, capture/reset buffer, then RememberFirstError(WriteAll). Append, Flush,
Sync and Close reuse it without adding a close_attempted policy Windows does not
currently have. Close still attempts handle close after flush and returns flush
failure first.

Windows intentionally has no uniform terminal state. Characterize the existing
matrix rather than imposing POSIX policy: after successful Close a small Append
may buffer; an empty Flush may succeed; Sync/second Close reach the invalid
handle and latch/return I/O failure. After a reported close failure, another
Close attempts the invalid handle again. Preserve exact operation call counts.

## Directory sync

POSIX SyncDirectory opens a directory descriptor, calls
SyncDescriptor(data_only=false), always attempts CloseDescriptor, returns the
sync error before close error, and otherwise returns close status.

Windows validates/encodes the path first. Strict mode rejects with NotSupported
before directory acquisition. Weak mode opens/validates the directory and only
closes it; it does not claim a strict namespace sync syscall. Preserve path,
consent, acquisition and close-error priority separately.

## Native locks

Extract platform-local lock acquisition leaves only where they own syscall/error
classification. POSIX process registry reservation, descriptor acquisition,
fcntl lock, owner construction and reservation commit stay visible in LockFile.
Windows OpenRegularFile, LockFileEx, Busy translation and handle transfer stay
visible. Unlock remains destructor-owned and best effort.

For POSIX teardown, add a child-process acquisition after owner destruction to
prove kernel lock/descriptor release, plus same-process reacquisition to prove
registry release. Windows retains its existing child-process proof.

## Characterization

Before production edits, strengthen native cases for the platform-specific
post-Close matrices, destructor best effort, exact first Error copies including
source location/independent ownership, directory-sync ordering, lock contention,
handle/descriptor release and retry after failed acquisition.

Use deterministic existing seams where available. `/dev/full` supports POSIX
write failure, while Linux `/proc/self/fd` supplies a directory-sync failure
whose descriptor release can be observed through `/proc`. Combined sync/close
faults, direct POSIX close and fcntl nonstandard failures, and Windows
LockFileEx/UnlockFileEx noncontention branches remain explicitly inspection-based
unless a platform-local production seam is independently justified. Do not add
broad syscall abstraction solely to manufacture coverage. Run actual POSIX and
Windows native suites; one platform cannot stand in for the other.

Independent design and implementation review precede one non-draft PR. Exact
hosted native identities, all CI gates and reviewed-path equality are required.
This batch is not complete native-platform or production certification.

## Independent design review

Review corrected directory-sync scope, platform-specific post-Close behavior,
POSIX closed-before-sticky priority, source-location/owner equality, limits of
current fault seams, and the need for a POSIX child-process kernel-lock release
oracle. These contracts are incorporated above before production edits.

## Local implementation evidence

Original POSIX characterization commits `f581e4c` and `d9f348f` cover
closed-before-sticky diagnostics, complete Error context/independent ownership,
child-process kernel unlock and Linux descriptor-count release.

Production commit `022ce85` adds POSIX caller-location-aware CheckOpen and
extracts the unchanged Windows Flush body without adding a close state.
WindowsFileSystemTest, WalIoNativeTest, WalWriterTest and DatabaseTest pass
locally. Independent implementation review found no significant issue.
Linux/macOS runtime evidence remains the hosted CI delivery gate.
