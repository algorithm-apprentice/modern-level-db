# Trivial background moves and WAL rotation composition

## Status and scope

Design proposal for A02/A03 from the
[production composition plan](production-function-composition-plan.md).
Scope is DatabaseEngine::Compact's trivial-move branch and SwitchMemTable's
replacement preparation/old-log close composition, plus private declarations
in `src/engine/database.h` and direct existing engine tests.
No public API, write-buffer installation, rewrite algorithm, MANIFEST/WAL format,
native I/O policy, diagnostics or benchmark changes.

The compaction output/history #194 and input expansion #196 implementation
paths do not overlap these engine/header/test paths. This design does not edit
their shared ledger; batches can be published independently.

## A02: one trivial-move installation

Extract private `InstallTrivialCompaction(const Compaction&, unique_lock&)`.
Compact retains its pinned current version, picked Compaction, IsTrivialMove
decision and return false after the child.
The child composes existing CompactionEdit, adding the unchanged original file
to its next level, checked LogAndApply with the same owned lock, and existing
FinishCompaction sticky-error/seek-budget handling.

Do not change `false` to application success: it means the task used a trivial
move rather than rewritten outputs. BackgroundCompaction still erases tracked
output numbers and runs obsolete-file cleanup only when Compact returned true.
Failed or successful trivial moves both return false with no output rewrite,
new output number, table I/O or cleanup removal.
Keep the original temporary LogAndApply Status passed directly to
FinishCompaction, with the same lifetime and no new copies.

The borrowed Compaction stays in Compact through the child and contains a
pinned version. Its AddFile uses the existing trusted valid topology; ignored
checked AddFile status is unchanged, not a newly invented error fallback.
Mutex unlock/relock in VersionSet::LogAndApply remains visible through its
owned-lock argument and existing exception contract.
Rewrite setup, output hooks and unlocked RunCompaction remain later recursive
frontiers, not certified here.

## A03: prepare, close, install and schedule

Introduce private `PreparedRotation` aggregate holding owned replacement
WalWriter, shared mutable MemTable and the allocated log number. Declare fields
writer then mutable table then number, so failure cleanup releases mutable table
before writer, as the original local declarations do.

`PrepareRotation` returns Result<PreparedRotation> and composes:

1. Allocate existing VersionSet file number and build log path.
2. Open replacement writable file, retaining the existing moved error.
3. Allocate WalWriter and mutable MemTable, in that order.
4. Run existing optional directory namespace barrier.
5. Return the named replacement aggregate without installing live state.

No new WAL Sync is added: original rotation's optional barrier is the directory
sync only. Recovery's creation policy is not substituted for rotation.
Opening/allocating/reserving a file number can leave a name before later failure;
do not remove it or roll the counter back.
Open/directory status failure leaves original log/memtable intact, and later
writes may retry. The directory error retains its original Error copy inside
the helper; the new parent propagation moves. Allocation exceptions propagate
without sticky-error conversion because old Close has not begun.

`CloseOldLogForRotation` returns void and contains the exact original Close
try/catch/status policy. A returned error records the original sticky Error
but permits replacement installation. A thrown Close records
`Aborted("old WAL close threw during rotation")` then rethrows; replacement
owners unwind and live state is not installed. Do not catch or translate other
stages, suppress the exception, retry Close, or return a failure from this child.

SwitchMemTable becomes the composition:
PrepareRotation -> check/move preparation error -> CloseOldLogForRotation ->
existing WriteBufferState::InstallRotation -> MaybeScheduleBackgroundWork ->
original successful Status.
A returned close failure still installs the new generation, but sticky error
blocks scheduling/next write via existing callers. A thrown close leaves the
old generation installed. Both are deliberately different.

Hold the original database mutex through preparation, Close and installation.
Replacement aggregate owns both new objects until transferred; it remains
alive across Close. No borrowed MemTable pointer escapes.
Original pathname temporary now ends with preparation rather than rotation;
it owns no native/file/engine resource. Do not shorten writable file, log,
mutable/immutable, lock or pin lifetimes to simplify the helper.
WriteBufferState::InstallRotation retains its requires-Close-returned contract
and atomic publication order. It is not reimplemented here.

## Recursive ledger and limits

| Definition | Intended domain responsibility |
|---|---|
| `InstallTrivialCompaction` | Compose the original move edit and application/finish policy. |
| `Compact` | Compose move versus existing rewrite orchestration; rewrite remains a frontier. |
| `PrepareRotation` | Acquire replacement generation before live mutation. |
| `CloseOldLogForRotation` | Apply existing returned-error versus thrown-close sticky policy. |
| `SwitchMemTable` | Compose preparation, old Close, original installation and scheduling. |

Private owner/type declarations support the preparation transaction, not new
public customization or a rotation framework. Existing engine/write-buffer
children, constructors, hooks and lock-sensitive algorithms remain subsequent
recursive frontiers.

## Characterization before extraction

Existing DatabaseTest cases cover trivial move failed MANIFEST then retry,
unchanged table numbers/no .ldb operations or remove calls, rewritten-output
failure cleanup, close-during-output and immutable flush interactions.
Retain them and strengthen trivial-move error code/state/protected-output count
checks, so return false is not confused with failed installation.

Rotation already checks exact open/barrier/old-close/new-append sequence with
the optional barrier disabled/enabled, replacement-open/barrier retry,
returned Close error, thrown Close, memory visibility and queued background work.
Strengthen GetState before/after to distinguish prepared but uninstalled errors,
installed generation after returned Close error, and unchanged generation after
throw. Assert immutable presence, published last sequence, sticky error tuple,
and no new log Append for a rejected write. Use existing state view and
filesystem logs, not production tracing or exposing private owners.

Retain replacement status failures as retryable and preserve returned-error
versus throw distinction. Native destruction/allocation injection are not
implicitly measured by MemoryFileSystem logging.
Run the real DatabaseTest suite with exact unique JUnit before/after parity,
new/strengthened names present and no failures/errors/skips. Rebuild actual
modern_leveldb_unit_tests; run unchanged format/docs/whitespace checks and
authoritative exact-head CI/coverage. No new exclusions or gate reductions.

## Review and delivery

Independent design review precedes accepted merge; characterization precedes
production edits. Rotation and trivial-move implementation are separate small
topic commits in one coherent engine PR, followed by independent combined
review and exact-head successful checks before normal merge.
Whole-production recursive completion and the final both-platform audit remain open.

Independent design review found no significant issue after inspecting current
engine/write-buffer source and related tests. It did not execute builds/tests.
State assertions supplement the original ordering/visibility/retry oracles,
not substitute for them.
