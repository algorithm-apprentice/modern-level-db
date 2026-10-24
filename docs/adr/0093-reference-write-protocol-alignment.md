# ADR-0093: Reference-Driven Write Protocol Alignment

- Status: Implemented

## Purpose and scope

Audit and align the complete write protocol, including background work,
rather than admitting a mechanism because it has fewer calls or a favorable
timing result. The current baseline is Modern revision
`56f7cb8086bfe4baf0f2a46bb7b395983798361e`.

Google LevelDB revision `7ee830d02b623e8ffe0b95d59a74db1e58da04c5`
is the primary serialized-write/leveled-maintenance reference. RocksDB
revision `553b52b4a709820fac1dbadc7796506740517582` supplies compatible
buffered-file and terminal-error mechanisms. This does not import column
families, unordered or pipelined writes, parallel MemTable insertion,
transactions, disabled WAL, or recycled log formats.

The claim is bounded to this embedded engine's documented contract and
admitted filesystem/failure model. It is not a promise to satisfy every
possible storage engine's requirements or a formal proof of the entire codebase.

[ADR-0060](0060-leveldb-write-path-parity.md) establishes behavioral
invariants; [ADR-0092](0092-native-write-cost-model-alignment.md) implements
bounded producer-cost alignment. Neither a local benchmark nor a Flush
count alone proves this complete protocol.

## Required event and failure dependencies

The successful foreground dependency is:

```text
stable owned batch and queue-front ownership
    -> compatible group and reserved sequence interval
    -> complete WAL record submitted to the operating system
    -> requested WAL content barrier, when sync is true
    -> every MemTable entry inserted
    -> final sequence published under the database mutex
    -> complete exactly the admitted writers
```

Non-sync writes still await the successful call; they do not request the
extra per-write WAL content barrier. File/name durability remains subject
to the platform reference, including explicit Windows weak namespace mode.

The background representation dependency is:

```text
retain input owners and protect output numbers
    -> write and finish complete sorted output files
    -> content Sync and Close succeed
    -> required output-name barrier
    -> durable MANIFEST/CURRENT installation
    -> publish the new Version
    -> retire old sources only after their readers and recovery no longer need them
```

An uncertain side effect changes the permitted state transitions:

```text
uncertain WAL/metadata/maintenance failure
    -> latch terminal failure without allocating a diagnostic
    -> no new foreground writes, background admission, or obsolete-file cleanup
    -> restore lock/sequence/queue obligations
    -> finish accepted-task accounting and notify waiters
    -> report the best available failure information
```

Recording an error message is not allowed to be the operation that decides
whether the engine is failed.

Admission is distinct from settling an already-running operation. A
background flush or compaction admitted before a foreground WAL error may
finish installing its independently protected committed history; existing
tests deliberately cover that behavior. No new task is admitted and no old
files are newly selected for reclamation afterward. A deletion batch already
selected under the mutex may finish: its unique, never-reused names were
proven obsolete before the error. A metadata owner's own uncertain persistence
result instead blocks every later edit. The audit must verify these
phase-specific rules rather than claim that a latch cancels all in-flight
I/O.

## Full-protocol audit obligations

Each row needs current source, pinned reference behavior, deterministic
tests, and a disposition for differences. Mapping a source entry point is
not by itself completion evidence.

| Phase | Required invariant | Modern responsibility | Reference responsibility |
|---|---|---|---|
| API/batch | Stable bytes, valid records and ordered updates; explicit exclusive borrow versus const copy | API and batch codec | Native WriteBatch/DB write boundary |
| Queue/group | One leader; compatible sync and size membership; stack writers remain alive; exact completion | WriteQueue | Native BuildBatchGroup/Writer; RocksDB WriteThread |
| WAL | Byte-compatible fragments/checksums; complete logical submission before visibility; stop after uncertainty | WalWriter and writable backend | Native log Writer; RocksDB WAL/file writer |
| Publication | No visible partial batch; one reserved sequence interval; reader/source synchronization | CommitWrite and MemTable | Native DBImpl::Write/MemTable insertion |
| Rotation | Replacement owners prepared before swap; old-close uncertainty is terminal; no lost accepted task | WriteBufferState and scheduler | Native MakeRoomForWrite; RocksDB write preparation |
| Flush | Preserve immutable history until complete durable installation; placement respects overlaps | BuildTable/FlushImmutable | Native WriteLevel0Table/CompactMemTable |
| Compaction selection | Include required overlapping/boundary inputs; deterministic level topology | Compaction picker | Native PickCompaction/SetupOtherInputs |
| Compaction execution | Sorted merge, protected snapshots, no resurrection, bounded outputs/overlap | RunCompaction | Native DoCompactionWork; compatible RocksDB retention rules |
| Output buffering | Buffering and Flush may differ only with preserved completion, error, and installation obligations | TableBuilder/writable file | Native block Flush; RocksDB buffered writer |
| MANIFEST/CURRENT | Prepare before I/O; retain old topology on uncertainty; no allocating publication after durable success | VersionSet | Native LogAndApply; RocksDB metadata error handling |
| Reclamation | Keep every live/pinned/pending/log/manifest dependency; no cleanup after uncertainty | RemoveObsoleteFiles | Native RemoveObsoleteFiles |
| Shutdown/recovery | Accepted work reaches terminal accounting; no stranded waiters; replay complete valid history | BackgroundCall, destructor, recovery | Native background/destructor/recovery ordering |

The output-buffer difference is reopened as a protocol question, not
preapproved by its measured speed. A compatible RocksDB-style buffering
choice needs the complete Sync/install/error argument. Native's per-block
Flush is the conservative reference when that argument is absent.

## Audit dispositions and executable witnesses

The source audit follows the complete call paths, including their unlocked
intervals and failure exits. The seven inspected Native protocol files were
matched against the pinned repository's blob identities; a parent repository's
`git rev-parse` result is not reference provenance for an extracted archive.

| Phase | Current trace and pinned comparison | Deterministic witnesses | Disposition |
|---|---|---|---|
| API/batch | `src/api/database.cc`, owned batch codec; Native WriteBatch and DB write boundary | Public API batch tests; `InsertBatchTest`; batch codec tests | Preserve const-copying Write and explicit exclusive borrow. Validated owned bytes enter trusted iteration; external WAL bytes do not. |
| Queue/group | `write_path.h/.cc`; Native Write/BuildBatchGroup | `GivesEveryWriterOfAGroupItsStatus`, exact size-limit and sync-membership cases, forced-barrier cases, exhausted leader/follower case | One front writer; compatible groups, Native size limits, sequence restoration and exact completion. Forced writers deliberately complete alone. |
| WAL | `wal_io.cc`/fragmenter/native writable files; Native log Writer | WAL format/golden/native tests, `CommitGroupTest`, partial-prefix allocation cases | Byte-compatible logical/physical records. One complete-logical-record Flush is retained with equivalent acknowledgment ordering. Every uncertain commit failure stops later admission. |
| Publication | `CommitWrite`, MemTable links and published sequence; Native DBImpl::Write | `ReadersCannotObserveAPartiallyInsertedBatch`, state-during-insertion, fitting-retry/sequence-limit and recovery-domain cases | Publish only a fully inserted group. Prove the 56-bit interval at commit/recovery boundaries; no per-entry revalidation. |
| Rotation/stalls | `MakeRoomForWrite`, prepared rotation, `WriteBufferState`; Native MakeRoomForWrite | WAL-create/barrier/Close failures, immutable wait, L0 slowdown/stop, schedule/re-schedule fault cases | Same trigger order and generation serialization. Prepare replacement ownership first, retain stronger default WAL-name durability, and never reuse failed file numbers while open. |
| Flush | `BuildTable`, `FlushMemTable`, `FlushImmutable`; Native WriteLevel0Table/CompactMemTable | Flush placement and all-operation failure matrices; immutable/source protection; late Sync/Close/name failures and reopen | Preserve the immutable and recovery WALs until complete output and metadata installation. Same overlap/grandparent placement through level two. |
| Compaction selection | picker, pinned Version, trusted version delta; Native PickCompaction/SetupOtherInputs | L0 closure, boundary-key chain, expansion budget, expanded-next boundaries, whole-range grandparents, reverse comparator | Same leveled selection, expansion and trivial-move rules. The compact pointer publishes with the successful edit rather than before a failed terminal operation. |
| Compaction execution | merge input, VisibleHistory/BaseLevel/GrandparentOverlap/Outputs; Native DoCompactionWork | Snapshot/deletion retention, dropped-entry grandparent progression, target-size boundaries, corrupt-input and final-move failure, immutable-priority cases | Preserve required history, prevent resurrection, bound outputs, and flush immutables between entries. Intact malformed internal input fails closed rather than being copied into a new table. |
| Output completion | TableBuilder/native writable buffering, output open verification and namespace barrier; Native builder and RocksDB buffered writer | Table finish counts, native buffered-write failures, late composed output failures, power-loss cuts | Compatible buffering separation accepted on dependency/error evidence, not call counts. No incomplete/failed output is normally installed. |
| Metadata | prepared candidate/pointers/snapshot/encoding/capacity, unlocked persistence, ownership-swap installation; Native LogAndApply | Direct/locked uncertainty, safe staging retries, late raw pins, counter advancement, threshold-zero publication and final-CURRENT-barrier cases | Old topology remains authoritative on uncertainty; no later metadata edit. Successful publication does not allocate or overwrite advanced foreground counters. |
| Reclamation | pinned/live/pending set under mutex, then unique-name deletion; Native RemoveObsoleteFiles | Iterator/raw-read pin retention, protected output/state cases, failed metadata retention, preselected-deletion completion and reopen cleanup | No new selection after terminal uncertainty. Previously selected names stay obsolete; best-effort failed deletions can be retried on a later open. |
| Shutdown/recovery | accepted callback accounting, closing checks, retained dependency owners, checked WAL replay; Native background/destructor/recovery | Pending/running/late-output close cases, exhausted task epilogues, every-recovery-operation failures, model/cross-open/power-loss/process-crash suites | Drain accepted engine work before releasing dependencies. Recovery accepts complete valid history, skips damaged physical records/tails by the existing policy, and rejects intact invalid batches or metadata. |

This completes the current-product code/dependency review with named
executable witnesses. The required delivery matrix still includes the
ordinary platform, coverage, compatibility and sanitizer jobs; source mapping
or local Windows results alone are not substitutes for those checks.

## Confirmed terminal-state gap

A deterministic current-source probe first writes an acknowledged seed,
then writes one byte of the next WAL header and injects allocation failure.
Both a thrown allocation exception and a returned I/O failure whose later
diagnostic copy allocates can reach:

```text
partial WAL bytes exist
write exits with bad_alloc
engine has no sticky error
another write succeeds and appends to that uncertain WAL
```

The Release probe reproduced both modes. An ordinary-allocation failure
also reproduced the thrown mode in Debug. Debug's returned-error case has
additional standard-library allocation behavior; its initial stall is not
used as proof of a separate engine cause.
Keep threshold-zero exhaustion evidence separate from targeted owning-
diagnostic allocation failure; they are not interchangeable experiments.

Baseline engine catch blocks constructed an owning Aborted message before
calling RecordStickyError. Ordinary failure recording also copied Error
before entering that method. Baseline VersionSet assigned an owning Error after
metadata I/O, while scheduling/background catch blocks could allocate before
clearing accepted-work state.

These are related instances of one failure-transition problem. A fix must
cover the owner state, diagnostic allocation, queue completion, background
accounting, and metadata installation rather than patch one error string.

## Failure-state correction

Use the mature separation between a terminal signal and its diagnostic,
as exemplified by RocksDB's seen-error state:

- prepare an owning fallback error before the owner can mutate storage;
- latch its already-owned handle before attempting detailed error creation
  or copying;
- keep the first failure and preserve the original detailed Error when
  allocation succeeds;
- retain an explicit Aborted diagnostic if allocation of details fails;
- never allocate to retain the fallback or wake terminal-state waiters;
- do not change success-path ordering, bytes, or durability defaults.

Exception catch sites pass nonowning context to the latching operation,
not a newly allocated Error argument. Shared immutable error ownership can
make this transition allocation-free even when a library's string moves
have additional Debug bookkeeping.

Prepare/validate/encode before entering metadata persistence where possible.
Exceptions in that safe staging phase remain retryable. Once persistence
starts, a thrown exception or returned failure must protect the owner from
later edits even if its caller is not DatabaseEngine.
The old metadata test that retried after an Append hook threw must not
serve as evidence of a safe staging failure: the real backend cannot promise
that no bytes were written. Retryability is tested before persistence instead.

A bounded independent design review approves the latch primitive with
these limits, not the full-protocol completion claim. Native's diagnostic
copy is not an allocation-failure safety proof; RocksDB's nonallocating
seen-error signal supplies the relevant separation without importing its
broader severity/recovery policy.

`TerminalError` in `src/base/result_internal.h` implements that private
owner-synchronized primitive. DatabaseEngine and VersionSet prepare it during
construction, before mutating storage. Returned failures enter `Record` by
reference; catch sites enter `RecordException` with nonowning context. Both
install the fallback handle before copying or constructing details and catch
specifically `bad_alloc`. Detailed diagnostics and their original call sites
remain unchanged when allocation succeeds.

VersionSet stages the encoded edit with the candidate Version, compact
pointers, new-MANIFEST snapshot, and publication capacity before persistence.
Both direct and lock-aware persistence exceptions latch the metadata owner;
the latter restores the database lock before recording and propagating.
Observer copies may throw, but they cannot clear or replace the first latch.
No public Result/Error representation, successful WAL/SST bytes, or default
durability policy changes.

The initial deterministic matrix had nine invariant failures and one safe
staging retry before the correction. The expanded isolated matrix covers
partial WAL and MANIFEST effects, MemTable failure after a complete WAL,
rotation Close, scheduling and rescheduling, background completion,
grouped-follower wakeup, repeated diagnostics and failed observers, and
allocation-free publication after successful MANIFEST sync. Threshold-zero
ordinary exhaustion and the returned-error diagnostic threshold of 32 bytes
remain explicitly distinct injection modes.

## Confirmed post-persistence publication gap

The next fault boundary exposed a second defect. After an existing MANIFEST
Sync succeeded, ordinary allocation exhaustion while publishing a previously
absent compact pointer terminated the supported MSVC Debug process. A captured
stack, rather than a noexcept/type-trait inference, identified:

```text
PublishPreparedEdit
    -> array<optional<InternalKey>>::operator=
    -> construct a newly engaged InternalKey
    -> vector<byte> move constructor
    -> allocate a Debug container proxy
    -> bad_alloc inside a noexcept move
    -> terminate
```

Moving staged container elements therefore did not establish allocation-free
publication. Disabling iterator checks or weakening the fault threshold would
hide the defect rather than correct the protocol.

Prepare a separately owned complete compact-pointer array before I/O, then
publish by swapping its unique ownership. Retain the existing span accessor's
shape, with an explicit borrow lifetime ending at the next successful edit.
The picker consumes that span under the mutex and does not retain it.
The old array remains authoritative until persistence succeeds; a new-MANIFEST
snapshot still encodes the old pointers and topology, followed by the new edit.

The successful publication suffix now exchanges ownership, publishes captured
log fields, and installs the already-owned Version into reserved tracking
capacity. It neither moves vector-backed optional elements nor restores stale
copies of foreground sequence/file counters.

An independent bounded design review approved this ownership swap over
double inline arrays or a broad InternalKey representation change. Its required
tests include threshold-zero direct and lock-aware publication, a raw read pin
arriving during unlocked I/O without another strong old-Version owner,
foreground counter advances, and recovered/new-MANIFEST publication after the
final CURRENT namespace barrier. The corrected Debug cases pass without the
temporary terminate hook.

## Sequence-domain boundary correction

The audit also reproduced acceptance of a checksummed WAL batch whose sequence
exceeded the internal key's 56-bit domain. A running engine recovered just below
the limit likewise acknowledged a two-entry group that crossed the limit.
Trusted trailer packing then truncated the high sequence bits. Both regression
tests failed before correction; this was not an attribution from timing.

Check the entire reserved interval once before WAL commit, and validate decoded
WAL intervals before trusted recovery insertion. An oversized foreground group
returns Aborted without appending or advancing visibility; because this is a
known pre-persistence rejection, a smaller fitting group can still succeed.
After the final representable update, nonempty groups are rejected. An empty
batch may carry the next sequence token, `MaxSequenceNumber + 1`, while using
no sequence and leaving the published last sequence at the maximum.

Recovery rejects an out-of-domain first sequence, a crossing interval,
uint64 overflow candidates, and an out-of-domain empty token without creating
outputs or installing metadata. The last valid interval and its empty successor
remain recoverable. This strengthens Native's practical unchecked arithmetic
at explicit boundaries; it does not add redundant guards inside MemTable
insertion or change representable WAL bytes.

## Output buffering: complete protocol argument

Retain the buffered SST policy, not because it has fewer explicit calls, but
because its dependency chain is preserved:

1. `TableBuilder::AddTrusted` completes ordered data blocks through Append.
   A block boundary need not drain the writable file's private buffer.
2. `TableBuilder::Finish` writes the remaining blocks and footer, then requires
   successful file Sync and Close. Both owned native backends drain their
   buffer before requesting the content barrier.
3. `BuildTable` and compaction output completion open the completed table;
   flush/compaction require the output-name barrier before producing an edit
   that may be durably installed.
4. VersionSet persists the edit before publishing its topology. The immutable
   MemTable, input Versions, pending output numbers, and recovery WALs remain
   protected through the required phases.
5. Append, deferred buffer draining, Sync, Close, namespace, or metadata failure
   prevents normal installation/reclamation and reaches the owner failure
   policy. An unused failed output may be discarded; that is not authorization
   to delete a recovery source.

Pinned Native `TableBuilder::Flush` also invokes the underlying file Flush at
each data-block boundary. Pinned RocksDB separates a block builder's Flush from
its buffered file writer's Flush/Sync lifecycle. Modern retains that compatible
separation, not RocksDB's complete writer, severity, or recovery policy.

The same reasoning permits Modern's one underlying WAL Flush per complete
logical record instead of Native's per-physical-fragment Flush: acknowledgment
still follows complete submission, requested Sync, and whole-group insertion.
An interrupted fragmented record is not a partially visible batch.
No Flush count establishes a syscall count or a performance improvement.

The executable witnesses include `TableBuilderTest.FinishSyncsAndClosesTheFileOnce`,
the per-file-operation failure matrices in BuildTable/Flush/Compaction tests,
native writable-buffer tests, and
`DatabaseTest.LateTableCompletionFailuresKeepTheImmutableAndItsRecoveryLogs`.
The latter checks Sync, Close, and output-name failure at the composed engine
boundary and then reopens through the retained WALs.

## Admission, completion, and reclamation

The database mutex protects queue membership, topology, generation swaps,
failure state, live-set selection, and task accounting. Queue-front ownership
protects the mutable WAL/MemTable during unlocked commit; the published
sequence hides newer partially linked entries from readers. A background task
pins its input Version and protects each output number before unlocked I/O.

Only one background metadata installer runs. A compaction may synchronously
flush a newly immutable generation between input entries, then apply its
original input deletions to the latest Version. The pinned inputs still exist;
new flush additions are not replaced by reinstalling the old whole Version.
Snapshot and tombstone retention use the compaction's pinned topology and
oldest required visibility boundary.

Failure gates apply at admission boundaries, not by pretending to cancel
already-running I/O:

- an admitted flush can install protected history after a foreground WAL error,
  but cannot start a new obsolete-file selection afterward;
- a metadata operation already persisting may settle while shutdown starts;
  an output not yet admitted for installation is rejected if closing is set;
- a deletion batch selected before a foreground error can finish because
  its names were already proved obsolete and file numbers are not reused;
- an uncertain metadata owner's later edits are always rejected, including
  standalone VersionSet use outside DatabaseEngine.

These distinctions are exercised by
`KeepsObsoleteFilesAfterABackgroundError`,
`FinishesOnlyTheCleanupBatchSelectedBeforeAForegroundError`,
the late-pin/counter publication tests, shutdown-at-output-boundary tests,
and allocation-failure task-epilogue tests. They match Native's serialized
installer and unlocked deletion model rather than an unsupported cancellation
guarantee.

## Verification requirements

Use isolated allocation-fault tests so global allocation replacement does
not affect unrelated unit tests. Arm failure only at the required transition,
after setup and after a real partial operation where applicable. Test both
ordinary allocation exhaustion and failure of owning diagnostic allocations.
Successful children restore allocation and use normal process exit so coverage
and allocator exit hooks run. A detected invariant violation exits immediately:
its injected failure or broken task state must not turn the negative test into
an unbounded shutdown.

Required cases include:

- uncertain WAL exception and returned failure, followed by a rejected write
  with unchanged WAL and published sequence;
- old-WAL close exception and scheduler rejection/throw during rotation;
- background table and metadata failures, with terminal task accounting,
  no rescheduling/cleanup, retained logical sources, and non-hanging shutdown;
- standalone and lock-aware VersionSet uncertainty, restored lock state and
  rejection of later edits without new file I/O;
- retryable allocation failure before persistence and retained first-error
  diagnostics when later observations allocate or fail.

Then complete every audit row with reference traces, model/compatibility,
crash/fault, concurrent-reader, shutdown and sanitizer evidence. Keep
incremental commits and independent bounded reviews. Performance is not an
acceptance shortcut and is not a reason to retain an unverified protocol.

## Verification record and limits

The implemented source passed 1,150 ordinary unit selections in each local
MSVC Debug and Release configuration. The isolated allocation executable
contains 24 cases, including safe owner-allocation and compact-pointer-copy
failures before persistence. The final local extended Debug selection passed all
19 model, cross-open/golden, deterministic power-loss and native Windows
process-recovery cases after the sequence-domain corrections.

The original ordinary Debug selection additionally passed its CMake consumer
and required-test contracts. Ordinary delivery CI retains the unmodified
GCC 13 changed-branch-coverage gate, Linux/macOS/Windows Debug and Release
jobs, portable CRC, compatibility, ASan/UBSan, TSan, and existing tool/profiling
contracts. No test, warning, permission, trust, or sanitizer setting is relaxed
for these corrections.

The crash filesystem separates byte and name persistence and freezes mutations
after a cut. Those traces and native process termination are evidence for their
stated models, not real power-loss/device certification. Ordinary allocation
exhaustion does not model every allocator, fatal page fault, broken mutex,
violated callback/executor contract, or external mutation of a live database.
The persistent uint32 key/value/batch representations and documented platform
admission limits remain unchanged. This is a reviewed alignment of the current
embedded-engine protocol, not equivalence to every RocksDB feature or a proof
that no storage-engine defect can remain.

## References

- [Pinned Native write/background protocol](https://github.com/google/leveldb/blob/7ee830d02b623e8ffe0b95d59a74db1e58da04c5/db/db_impl.cc)
- [Pinned Native metadata installation](https://github.com/google/leveldb/blob/7ee830d02b623e8ffe0b95d59a74db1e58da04c5/db/version_set.cc)
- [Pinned RocksDB writer ownership](https://github.com/facebook/rocksdb/blob/553b52b4a709820fac1dbadc7796506740517582/db/write_thread.cc)
- [Pinned RocksDB terminal file-error state](https://github.com/facebook/rocksdb/blob/553b52b4a709820fac1dbadc7796506740517582/file/writable_file_writer.h)
- [Pinned RocksDB block completion](https://github.com/facebook/rocksdb/blob/553b52b4a709820fac1dbadc7796506740517582/table/block_based/block_based_table_builder.cc)
- [Pinned RocksDB metadata/write error policy](https://github.com/facebook/rocksdb/blob/553b52b4a709820fac1dbadc7796506740517582/db/error_handler.cc)
- [Platform and failure boundary](../reference/platform-support-and-durability.md)
