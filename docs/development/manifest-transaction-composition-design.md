# MANIFEST replay and persistence composition

## Scope

Design proposal for M01-M05 in the
[production composition plan](production-function-composition-plan.md).
Change VersionSet recovery, prepared persistence and new-MANIFEST acquisition
in version_set.cc, directly dependent private declarations and characterization
tests. No format, allocation policy, public API, benchmark or coverage changes.
Existing checked/trusted builders, ReadCurrent, InManifest, Snapshot,
InstallCurrent and prepared topology installation remain the shared primitives.
This batch is independent of cache admission source/tests and avoids the shared
ledger while another published implementation owns that file.

## Recovery composition

`OpenSelectedManifest` composes ReadCurrent, ParseCurrentFileContents, the
CURRENT-selected number limit and descriptor opening. Return a private
SelectedManifest with number and owned SequentialFile. Preserve missing CURRENT
as NotFound, malformed/out-of-range CURRENT as Corruption, missing descriptor
translation with its original message, and other descriptor errors unchanged.
No VersionSet/builder allocation or MANIFEST read before successful acquisition.
The CURRENT string is only a parsing owner: it may be released after acquisition;
no borrowed data from it is returned.

A private nested `ManifestRecovery` owns the original checked VersionBuilder and
optional next/log/sequence plus previous-log/largest-number accumulators.
Keep this object declared after the VersionSet owner and before WalReader, so
reader destruction precedes builder and partial set destruction.
Its constructor initializes the builder from the same empty Version; it is not
declared noexcept because builder initialization can allocate.

`Replay(WalReader&, VersionSet&)` handles reader error, end, physical corruption
and logical-record dispatch. `ApplyRecord(record, set)` decodes the edit,
checks comparator, applies the checked builder, updates compact pointers, then
accumulates counters. Keep the decoded edit alive through all those steps.
Checked builder failures use InManifest(record.offset, error); comparator
mismatch remains InvalidArgument without the MANIFEST-offset envelope.

Name `CheckComparator`, `UpdateCompactPointers` and `AccumulateCounters` only
where they represent actual responsibilities, not wrappers for one getter.
Counter accumulation first includes added file numbers, then checks decreasing
log and updates optional log/next/last, carries omitted previous-log forward,
and includes current/previous log maxima. Preserve this exact order and all
historically named maxima, including subsequently deleted files.
Do not make log/next/sequence monotonic beyond the original policy: only log
decrease is rejected; next/last retain the most recent present value.

`ValidateCounters` checks required next, log and sequence in that priority,
then next limit and largest-number-below-next. It performs no checked build.
`Finish(set, selected_number)` validates, builds the checked version with the
existing final "MANIFEST: " error translation, computes
`manifest_number = max(next, selected_number + 1)` and next allocation, assigns
recovered counters, then installs owned topology.
Return a Status; Recover returns its still-owned set only after success.
Recovery does not open a writer, write CURRENT, sync or delete files.

## Prepared persistence

`WritePrepared(edit, staged_snapshot, optional_lock)` names the original
temporary-unlock boundary. Without a lock, write normally. With a lock, unlock,
call Write, reacquire on normal return and in the original catch/rethrow path.
Do not use a destructor-based relocker that changes throwing lock behavior.
This helper adds no failure latch or conversion of thrown errors.

ApplyPrepared keeps filling edit counters, compact-pointer staging, snapshot
encoding, owned candidate allocation and ReserveInstall before calling this
helper. Snapshot captures existing topology/pointers under the mutex.
After reacquisition, returned I/O error latches failure and does not install
candidate topology/pointers/log counters. Thrown I/O errors reacquire and rethrow
without adding a VersionSet sticky error. Successful publication updates
compact pointers and log counters, not next allocation or last sequence:
foreground changes to those two counters while unlocked must survive.
Candidate and snapshot owners stay in the parent through I/O and publication.

`CreateManifest(snapshot_bytes)` opens the descriptor, constructs the WalWriter,
and appends the already staged snapshot. On error the local writer unwinds
before the caller reacquires its optional database lock, as originally.
Return an owned writer only on snapshot success. Parent Write owns the optional
new writer, appends the edit, syncs it, then for a new writer performs descriptor
directory sync, InstallCurrent, and manifest_ owner transfer in that order.
Do not add explicit Close, remove leftovers, change the snapshot source, encode
it while unlocked, or transfer manifest_ before CURRENT installation succeeds.
Existing manifests still use only edit append and sync.

Keep original error-copy sites inside extracted children. New Result/Status
envelopes propagate by move at the new call boundary; avoid adding a second copy
of Error by returning a const helper-result local from its parent.

## Definition responsibility ledger

| Definition | Job and recursive boundary |
|---|---|
| `OpenSelectedManifest` | Select and acquire recovery input through existing CURRENT primitives and native open. |
| `ManifestRecovery` constructor | Initialize one replay state from an empty checked builder. |
| `Replay` | Reader-event dispatch and logical-record composition. |
| `ApplyRecord` | Compose checked decoding, comparator, builder and recovered state updates. |
| `CheckComparator` | One compatibility decision preserving its error policy. |
| `UpdateCompactPointers` | Publish an edit's ordered last-wins compact pointers into the unreturned set. |
| `AccumulateCounters` | One record's ordered recovery accumulator transition and log-decrease policy. |
| `ValidateCounters` | Ordered consistency decision over the replay's required/historical counters. |
| `Finish` | Compose final validation, checked topology construction and recovered publication. |
| `Recover` | Acquire input, construct owners, replay and finish before returning ownership. |
| `WritePrepared` | One explicit write/unlock/relock transaction. |
| `CreateManifest` | Acquire and initialize the new writer with the staged snapshot. |
| `Write` | Compose snapshot acquisition, edit persistence and CURRENT selection before owner transfer. |
| `ApplyPrepared` | Stage allocation before I/O, then latch failure or publish under lock. |

ReadCurrent's reading loop, Snapshot's per-level materialization, Validate's
runtime checks, InstallCurrent's temporary-file transaction, and version
retention/read-pin helpers are later recursive frontiers. Completing this
bounded batch does not certify the whole metadata subsystem.

## Original characterization and validation

Existing VersionSetTest covers exact new/existing writer operation order,
snapshot bytes/counters, complete returned-I/O failure matrices, sticky failure,
recoverability before/after CURRENT rename, malformed/corrupt input, historical
maxima/decreasing logs, checked overlap rejection, concurrent counter changes,
staged snapshot under concurrent seek mutation, lock reacquisition and pins.

Add focused multi-fault recovery priority cases: malformed edit beats comparator,
comparator beats checked builder, builder beats decreasing log, required-counter
validation beats final overlap build. Assert exact code/messages and offset
envelope, not just broad Corruption. Strengthen selected-descriptor limit/missing
translation and early-stop I/O sequence with no recovery writes.
Add sparse multi-record counter/pointer carry-forward and last-present semantics,
including deleted historical file number rejection.
Strengthen thrown prepared-write oracle with unchanged topology/pointers/log
counters, reacquired lock, no new VersionSet failure latch, and subsequent retry
using a hook that throws before performing the first append.
This retry oracle makes no claim about safe retry after an arbitrary exception
that occurred after partial I/O.

Run new tests against original source before extraction. Build the real unit
target; run VersionSetTest, VersionTest and directly coupled RecoveryTest/
DatabaseTest as appropriate to changed ownership boundaries. Verify unique
JUnit before/after sets, required added cases and no failures/errors/skips.
Run formatting/docs/whitespace checks; independent combined review before
publication; all applicable exact-head CI checks must succeed before normal
merge. No exclusion or threshold changes.

Independent source-grounded design review found no significant issue. The
pre-append-only retry limit above incorporates its oracle caution. The reviewer
inspected recovery, persistence, checked builder and existing tests without
editing files or running builds/tests.
