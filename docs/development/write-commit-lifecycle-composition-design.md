# Write Commit Queue and Buffer Lifecycle Composition

## Scope and delivery

Design-first follow-up to the production function composition plan.
Source baseline: `ace9db65ba63827bdc154fef39d7ede0ff9ebd3b`.
This larger related batch covers durable group application, WriteQueue
admission/grouping/commit/completion, and WriteBufferState active/immutable
generation ownership. DatabaseEngine::Write, MakeRoomForWrite, CommitWrite,
rotation and flush remain real caller context but are unchanged.

Keep reviewed design, original-source characterization and responsibility-sized
production topics as local commits, then publish one non-draft PR. Changed
production paths are engine/write_path.cc/.h and
engine/write_buffer_state.cc/.h. Existing engine/database_write.cc remains
byte-identical. Tests stay in write_path_test.cc and
write_buffer_state_test.cc, with write-batch/WAL/recovery/database suites as
coupled behavior.

No public API, queue ordering, grouping limit, sequence reservation, WAL bytes,
sync policy, MemTable insertion, sticky-error policy, mutex ownership, atomic
memory order, allocator strategy, benchmark, runtime assert, diagnostic
conditional, coverage exclusion or threshold change.

## Existing transaction and exception boundaries

WriteQueue::Run owns one stack Writer until it is removed from the deque.
It queues under the caller-held database mutex and waits until either an earlier
leader completed it or it reaches the front. A completed follower returns the
shared Status published by its leader. A front writer creates LeaderGuard before
prepare; if prepare/grouping/commit throws, the guard reacquires the mutex when
necessary and completes exactly the writers it has taken on with the preallocated
Aborted Status.

A failed prepare or a forced writer completes alone. BuildGroup may allocate
while appending scratch group bytes; until group selection succeeds and
LeaderGuard::TakeOn is called, a throw affects only the leader. A multi-writer
Status owner is allocated after BuildGroup but before TakeOn and before commit,
so every taken-on follower can receive a result without allocation after the
group may reach the WAL. A single leader uses no shared-result allocation.

SequenceGuard restores the selected batch's prior hidden sequence on every
return or exception. Commit callbacks may release the database mutex. Run must
dismiss its guard before Complete removes stack Writer pointers, and Complete
must wake the next front writer after removing leader through last.

CommitGroup appends the complete encoded group, conditionally Syncs only after
successful append, and inserts trusted entries only after persistence succeeds.
DatabaseEngine turns failures into sticky errors and publishes mutable memory/
last sequence only after this function returns success; those caller stages are
unchanged.

WriteBufferState methods require the database mutex except the compaction hint.
Rotation installs the new log identity first, moves the old mutable owner to
immutable, release-publishes the hint, then installs the new mutable owner and
its measured bytes. ClearImmutable may move a read-pinned immutable into the
retention vector; the relaxed false store follows that ownership decision.
Cleanup removes only retained tables whose read-pin count reached zero.

## Topic one: durable group persistence and trusted application

Add PersistGroup(group, sync, log), composing the original AddRecord, append
error return, optional Sync, and sync error return. It does not open a reader,
insert entries, translate errors, catch exceptions or change whether false sync
skips the call. Retain the original const append/sync Status locals and their
single failed-Error copy at this persistence boundary.

CommitGroup retains the original persistence-before-visibility parent:
PersistGroup first into a non-const local Status, then trusted reader
construction and InsertBatch. A failed local returns by NRVO/implicit move;
the new outer boundary must not copy/reconstruct Error or create a new source
location. Thus extraction adds no Error copy beyond the one original const
append/sync return copy. Physical move/elision/destruction remains
implementation-dependent. No entry can reach MemTable before persistence
success. InsertBatch remains the existing trusted remaining-entry loop and does
not revalidate or publish sequence.

## Topic two: follower selection and scratch-group materialization

Add GroupSizeLimit(leader_size), returning `leader_size + 128 KiB` for a leader
whose encoded size is at most 128 KiB, otherwise 1 MiB. Preserve ordinary
size_t arithmetic and the existing practical batch-size domain.

Add CompatibleFollower(leader, follower): a null-batch forced barrier never
joins, and a non-sync leader cannot absorb a sync follower. A sync leader may
absorb either. No size/read occurs before compatibility.

Add TryAddFollowerSize(size, follower_size, maximum_size), performing the
original addition before the strict `size > maximum_size` rejection. Equality
is admitted. No overflow guard or saturation policy is introduced.

Add AppendFollowerBatch(group, leader_batch, follower_batch). On the first
admitted follower only, clear group_, append the leader batch, and redirect the
selected group pointer; then append the follower. Preserve Clear's zero scratch
sequence, Append order and all throwing allocation behavior.

BuildGroup retains leader pointer/encoded size, maximum, last writer and ordered
deque traversal. It composes compatibility, size admission, scratch append and
last advancement. A rejected follower and all later writers remain queued.

## Topic three: queue admission, sequence restoration and commit ownership

Add WaitForTurn(lock, writer), which pushes the stack Writer pointer and waits
on its condition variable with the original `done || front` predicate. It
returns only while the Writer remains alive in Run.

Add CommitRestoringSequence(result, lock, batch, sync, commit), owning only
SequenceGuard and assigning the callback result into the caller-owned Status.
It adds no catch; SequenceGuard restores before any exception escapes.

Add CommitSingleWriter(lock, leader, group, guard, commit). It takes on the
leader, owns one local Status, invokes the shared sequence-restoring child,
dismisses the guard, completes leader-only with no shared result and returns the
local Status.

Add CommitWriterGroup(lock, leader, group, guard, commit). It allocates the
shared default Status before TakeOn, then invokes the same sequence-restoring
child into that owner, dismisses, completes leader through last with the shared
owner and returns its Status. Preserve this exact allocation/take-on order.

Run retains stack Writer construction, WaitForTurn, completed-follower return,
LeaderGuard lifetime, prepare call, failed/forced leader-only completion,
BuildGroup and single-versus-multi dispatch. No generic task abstraction,
heap Writer, catch, retry or additional status owner.

## Topic four: completion publication and next-leader wakeup

Add PublishFollowerResult(writer, result), assigning the shared owner, setting
done, then notifying that follower in the original order. It is never called
for the active leader.

Add WakeNextWriter(), notifying the new front only when the queue is nonempty.
Complete retains ordered pop from the original leader through last, delegates
follower publication, stops exactly at last, then invokes the next wakeup.
No removed writer pointer is read after its owner may return.

LeaderGuard constructor/destructor, TakeOn and Dismiss remain the exception
transaction. SequenceGuard constructor/destructor remain the batch-sequence
transaction. The preallocated aborted_ owner remains constructed once.

## Topic five: active and immutable generation ownership

Add InstallLogGeneration(log, number), moving the WAL owner then assigning its
number. Add InstallMutableGeneration(table), moving the mutable owner then
sampling its Arena memory usage into the published scalar. Both require the
database mutex and add no null guard.

Add PromoteMutableToImmutable(), moving the current mutable shared owner to
immutable then release-storing true to has_immutable_. It does not install the
replacement mutable owner.

InstallRecovered composes log and mutable installation. InstallRotation
composes log installation, old mutable promotion, then replacement mutable
installation in the original order.

Add RetireImmutable(), moving a read-pinned immutable owner into read_pinned_ or
resetting an unpinned owner. ClearImmutable calls it before the existing relaxed
false hint store. ReleaseReadPinnedMemtables retains its erase_if predicate and
does not reset still-pinned generations.

## Selected twenty-two-definition ledger

| Definition | Composition or stopping boundary |
| --- | --- |
| PersistGroup | New append/optional-sync durable Status stage |
| CommitGroup | Durable group before trusted MemTable application |
| GroupSizeLimit | New small/large leader grouping policy leaf |
| CompatibleFollower | New forced/sync barrier eligibility leaf |
| TryAddFollowerSize | New add-then-strict-limit admission leaf |
| AppendFollowerBatch | New first-follower scratch initialization and ordered append |
| WriteQueue::BuildGroup | Ordered compatibility/size/materialization selection |
| WriteQueue::WaitForTurn | New stack-writer queue admission/wait operation |
| WriteQueue::CommitRestoringSequence | New guarded callback result assignment |
| WriteQueue::CommitSingleWriter | New allocation-free leader commit/completion path |
| WriteQueue::CommitWriterGroup | New preallocated shared-result group path |
| WriteQueue::Run | Queue/prepare/select/commit parent orchestration |
| WriteQueue::PublishFollowerResult | New follower result/done/notify publication |
| WriteQueue::WakeNextWriter | New nonempty-front notification |
| WriteQueue::Complete | Ordered removal, follower publication and next wakeup |
| WriteBufferState::InstallLogGeneration | New active WAL owner/number installation |
| WriteBufferState::InstallMutableGeneration | New mutable owner/usage installation |
| WriteBufferState::PromoteMutableToImmutable | New owner move and release hint publication |
| WriteBufferState::InstallRecovered | Recovered log/mutable generation composition |
| WriteBufferState::InstallRotation | Log, immutable promotion and replacement mutable composition |
| WriteBufferState::RetireImmutable | New pinned-retention versus reset decision |
| WriteBufferState::ClearImmutable | Ownership retirement before relaxed false hint |

## Thirty-nine inspected unchanged named definitions

| Definition | Responsibility retained |
| --- | --- |
| InsertBatch | Consume remaining trusted entries and add each exact sequence/kind/key/value |
| LeaderGuard constructor | Borrow queue/lock/leader and initially cover leader only |
| LeaderGuard destructor | Reacquire when needed and abort exactly the taken-on range |
| SequenceGuard constructor | Capture one batch's prior sequence |
| SequenceGuard destructor | Restore the captured sequence |
| WriteQueue constructor | Preallocate immutable Aborted group result |
| deleted WriteQueue copy constructor | Keep mutex-adjacent queue identity stable |
| deleted WriteQueue copy assignment | Keep mutex-adjacent queue identity stable |
| deleted WriteQueue move constructor | Keep queued stack pointers bound to one queue |
| deleted WriteQueue move assignment | Keep queued stack pointers bound to one queue |
| defaulted WriteQueue destructor | Release scratch/result owners after no writers remain |
| WriteQueue::size | Read caller-locked deque size |
| deleted LeaderGuard copy constructor | Preserve one completion guard |
| deleted LeaderGuard copy assignment | Preserve one completion guard |
| deleted LeaderGuard move constructor | Preserve one completion guard |
| deleted LeaderGuard move assignment | Preserve one completion guard |
| LeaderGuard::TakeOn | Extend exception completion through selected last writer |
| LeaderGuard::Dismiss | Disable destructor completion after explicit Complete |
| deleted SequenceGuard copy constructor | Preserve one sequence restoration owner |
| deleted SequenceGuard copy assignment | Preserve one sequence restoration owner |
| WriteQueue::Write | Typed normal-writer adapter into Run |
| WriteQueue::Force | Forced barrier adapter with unreachable commit fallback |
| defaulted WriteBufferState constructor | Initialize empty generation state |
| deleted WriteBufferState copy constructor | Preserve unique WAL/generation ownership |
| deleted WriteBufferState copy assignment | Preserve unique WAL/generation ownership |
| deleted WriteBufferState move constructor | Keep borrowed state addresses stable |
| deleted WriteBufferState move assignment | Keep borrowed state addresses stable |
| defaulted WriteBufferState destructor | Release remaining WAL/table owners |
| WriteBufferState::has_log | Test active WAL owner presence |
| WriteBufferState::log | Borrow installed active WAL |
| WriteBufferState::log_number | Read installed WAL number |
| WriteBufferState::mutable_table | Borrow installed mutable MemTable |
| WriteBufferState::mutable_owner | Borrow active shared owner |
| WriteBufferState::immutable_owner | Borrow immutable shared owner |
| WriteBufferState::has_immutable | Test mutex-owned immutable pointer |
| WriteBufferState::HasImmutableForCompaction | Relaxed scheduling-hint load |
| WriteBufferState::published_mutable_bytes | Read mutex-published usage snapshot |
| WriteBufferState::PublishMutableMemoryUsage | Resample active MemTable Arena usage under mutex |
| WriteBufferState::ReleaseReadPinnedMemtables | Erase only retained generations with zero read pins |

The 22 selected/new definitions plus these 39 rows yield 61 post-change
named definitions in write_path.cc/.h and write_buffer_state.cc/.h.

## Three inspected unchanged lambda bodies

| Lambda | Responsibility retained |
| --- | --- |
| Force unreachable-commit fallback | Return the fixed Aborted diagnostic if a null-batch forced writer ever reaches commit |
| WaitForTurn predicate | Wake only when this writer is completed or becomes deque front while the mutex is held |
| retained-MemTable erase_if predicate | Select only generations whose read-pin count reached zero |

The complete bounded inventory is therefore 61 named definitions plus three
lambda bodies. DatabaseEngine write/rotation/flush callers remain
source-inspected integration context, not certified by this ledger. Counts are
bounded bookkeeping rather than a compiler-resolved call graph.

## Independent design review

Independent source-backed review found two gaps before implementation. The new
PersistGroup boundary now preserves the original one failed-Error copy inside
the const append/sync Status return and requires a non-const outer local with
NRVO/implicit-move propagation, preventing a second allocating copy or new
source location. The ledger now includes the forced fallback, wait predicate
and retained-generation erase predicate lambdas separately from the 61 named
definitions.

No tests, builds or production edits were performed during this bounded review.
Allocation failure, physical move/elision/destruction, scheduler behavior and
DatabaseEngine caller implementation remain future evidence.

## Original-source characterization

Existing WriteQueue tests already cover single direct storage, sequence restore
on success/error/throw, prepare failure, group result sharing, sync barriers,
size grouping, arbitrary incoming sequences, arrivals during commit, held/
unlocked exceptions, force barriers and high-concurrency exactly-once order.
CommitGroup tests cover append/sync failure suppression of MemTable insertion.
WriteBufferState covers recovered install, rotation and one pinned immutable.

Before production edits, add three cases:

1. Exact group-size boundaries for both small and large leaders. Construct
   one-record batches whose measured encoded sizes make the follower land
   exactly on the limit (admitted) or one byte beyond (left for the next group).
   Assert literal group order and no forced/sync confounder.
2. Prepare releases the mutex and throws while another writer is queued.
   LeaderGuard must reacquire, abort only the front writer, wake the follower,
   and leave the queue empty after that follower commits once.
3. WriteBufferState clears an unpinned immutable immediately, then retains two
   later pinned generations. Releasing one pin and cleanup must expire only that
   generation; the other retained owner and current generation remain valid.

Use public/internal subsystem operations, not private helper hooks. Prepare
batch key/value owners before threads and allocation-sensitive stages. Do not
infer general allocator-failure behavior from ordinary execution.

Freshly build modern_leveldb_unit_tests and the Windows native child on
unchanged production. Freeze exact unique identities for InsertBatchTest,
CommitGroupTest, WriteQueueTest, WriteBufferStateTest, WriteBatchTest,
WalWriterTest, WalIoNativeTest, RecoveryTest and DatabaseTest, with no
failure/error/skip. Independently review new cases before production edits.

Commit persistence/group selection, queue run/completion and buffer-generation
topics separately, checking exact relevant original subsets. Finish with full
identity parity and complete GCOVR/LCOV/NOLINT annotation multiset equality for
the four owned production paths. Local GCC Release warnings-as-errors should
compile all changed/coupled objects with `-fno-range-for-ext-temps`.

Independent integrated review must verify every ledger row, stack Writer
lifetime, lock ownership on every throw, result allocation/take-on order,
sequence restoration, status owner/copy behavior, exactly-once completion,
WAL-before-MemTable visibility, atomic hint order and retained MemTable owners.
One published exact head must pass all hosted checks, authoritative GCC 13
branch coverage and actual Windows Debug original-name parity before normal
squash delivery.

Successful tests do not prove arbitrary allocation failure, scheduler fairness,
lock freedom, physical copy/move/destruction counts, unsupported concurrent
writers outside WriteQueue, performance, or whole-production completion.
