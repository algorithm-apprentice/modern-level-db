# Metadata Required Fields and Prepared Publication

## Status and scope

Proposed recursive follow-up to the production composition plan, after
recovered-counter #238 and snapshot/liveness #225 delivery.
Source-read baseline: `6df11fc0edd02c915532a48269c5bd5c7de916cc`.
Pending key #245 uses independent source/test/design paths and is not a
prerequisite.

Scope: version_set.cc, directly dependent private version_set.h declarations,
existing version_set_test.cc and this document. Three small topics may share
one non-draft implementation PR: ordered required-field/bound proof;
compatible compact-pointer application and prepared-state stages; late
read-pin retention and current-version registration.
No public API, persistence/recovery/locking policy, allocation strategy,
coverage threshold/exclusion or benchmark changes.

## Required recovered fields before bounds

ManifestRecovery::ValidateCounters invokes RequireRecoveredCounterFields,
retaining a mutable local Status and returning it on failure, then retains
the original const next_file.value() and calls ValidateRecoveredCounterBounds.
RequireRecoveredCounterFields preserves next/log/last-sequence presence checks
and exact error literals in that priority. Only afterward can bounds reject
next > FileNumberLimit, then largest_number >= next with the original message.
Do not reorder existence/bound/checked-build errors or add unchecked .value()
reads. No default counter, rollback or no-value success fallback.
Finish keeps its existing ValidateCounters Status and Build Result lifetimes.
Added error-transport/scalar boundaries are not physical-copy/allocation
certification; return the mutable failed Status by normal implicit move.

## Real shared compact-pointer operation

Prior-art search finds identical ordered pointer-key assignments in the nested
ManifestRecovery::UpdateCompactPointers and ApplyPrepared's staged-array loop.
Replace them with one file-local ApplyCompactPointerEdits(edit, destination),
borrowing the existing array. The original checked builder application still
precedes replay pointer updates, which still precede recovered counters.
ApplyRecord retains const decoded edit and const builder status through the
shared call and counter accumulation. Remove the old nested helper rather than
leave a pure forwarding wrapper. No comparator/key copy optimization.
Staging still first copies compact_pointers_ into the parent-owned array;
ordered duplicate levels are last-present-wins in either destination.
The copy/assignment can allocate and throw as before; do not mark it noexcept.

## Prepared transaction parent and owners

ApplyPrepared retains its original const selected log/previous-log values,
edit owner, staged compact-pointer array, optional snapshot and shared candidate
through function exit. PrepareRuntimeEditCounters borrows edit and the two
selected scalars, performs the same SetLogNumber/SetPrevLogNumber then ignored
checked SetNextFileNumber/SetLastSequence calls in order, using existing trusted
state invariants. Do not invent new guards, assertions or error policy.

After the array copy/shared pointer updates, PrepareManifestSnapshot borrows
the parent's initially empty optional and retains the original manifest_ ==
nullptr gate. Snapshot uses original compact_pointers_/current_, not staged
pointers/candidate, before unlock. It materializes/encodes into the same parent
optional; no returned owner, transfer, callback framework or new container.
Candidate make_shared remains in the parent after the snapshot and before
ReserveInstall; capacity/ownership preparation remains before WritePrepared.

WritePrepared and its explicit unlock/try-relock/rethrow/relock behavior stay
unchanged. Failed written Status still latches failure_ and returns before
publication. PublishPreparedEdit borrows staged array/candidate and selected
scalars, moving pointers before log/previous-log assignment and the original
InstallPrepared candidate move. It never overwrites next_file_number_ or
last_sequence_, which may advance under the mutex during unlocked I/O.
Keep parent written Status/owners alive; no new noexcept for the publication
stage, catch, rollback, success-shaped write fallback or early installation.

## Late pin retention and registration

InstallPrepared retains its by-value shared candidate owner and noexcept.
RetainPinnedCurrent preserves current_ != nullptr && read_pins_ != 0 and
pushes the old current only then. ReserveInstall still reserves both existing
vectors before I/O; a newly acquired read pin during unlock changes pin count,
not the vector capacity invariant. Do not precompute retention before unlock.

Keep std::erase_if(versions_, held.expired()) directly in the parent: this
existing named operation and single-job lambda need no renaming wrapper.
RegisterCurrentVersion borrows the parent candidate, emplaces its weak entry
before moving it into current_, with the original no-allocation-after-durability
and noexcept contract. No new weak/shared/raw owners or pin policy.
ReleaseRead remains unchanged; ordinary pin destruction still requires the
same external synchronization as current changes.

## Twelve-definition ledger

| Definition | Children or responsibility-based stopping reason |
|---|---|
| `ValidateCounters` | Required-field proof, then original next scalar and bound proof. |
| `RequireRecoveredCounterFields` | One ordered presence contract with exact first-missing error. |
| `ValidateRecoveredCounterBounds` | One ordered namespace upper/historical-lower admissibility proof. |
| `ApplyRecord` | Existing decode/comparator/builder, shared pointer delta, existing counters. |
| `ApplyCompactPointerEdits` | Ordered complete key assignment to proven valid levels in a borrowed destination. |
| `ApplyPrepared` | Existing parent owners compose counter fill, pointer delta, snapshot, capacity/I/O, failed latch or publication. |
| `PrepareRuntimeEditCounters` | Fill one durable edit's selected log pair and contemporaneous next/sequence fields. |
| `PrepareManifestSnapshot` | Select and materialize the original current snapshot before I/O. |
| `PublishPreparedEdit` | Coupled staged pointer/log/topology commit after durable success; live next/sequence untouched. |
| `InstallPrepared` | Late pin retention, existing weak pruning, current registration. |
| `RetainPinnedCurrent` | Conditional old-current ownership needed by active raw read pins. |
| `RegisterCurrentVersion` | Coupled weak registration then strong current-owner transfer. |

The old nested UpdateCompactPointers is replaced, not additionally certified.
Existing erase predicate is one borrowed expiration test. Shortness is not a
stopping reason; Validate/Write/ReleaseRead and other children still participate
in the every-definition audit. Preserve all existing annotation text.

## Original-source characterization

Retain sparse/reset/history and builder/record priority cases, pre-append throw
and retry, unlocked-counter/current-install/snapshot cases, mixed shared/raw
pin liveness and runtime full-write-failure cases.
Add two public cases on unchanged source:

1. Presence/bounds/overlap matrix: an individually accepted overlapping file
   delta with all fields missing reports missing next; present beyond-limit
   next with missing log/sequence reports missing log; then present log with
   missing sequence reports missing sequence; all present reports beyond limit.
   A complete within-limit next not above the historical file bound reports the
   exact historical error before final overlap; raising next exposes overlap.
   SetNextFileNumber permits nonzero beyond-limit fixture values; do not forge
   raw malformed sequence fields or bypass decode.
2. Success/status-failure matrix for recovered new-MANIFEST publication:
   prepare duplicate staged pointer levels, replacement topology and explicit
   selected logs; block CURRENT temporary sync during unlocked I/O.
   Under the mutex acquire a new raw ReadPin to old current, verify old pointers/
   topology, advance file/sequence counters, then open/join/clear hooks before
   fatal assertions. Verify returned lock, exact captured durable edit fields,
   original snapshot pointers/files, delayed publication or sticky old state.
   Successful publication retains the late-pinned old version until synchronized
   pin release; failed publication leaves original current alive. Owned
   observations and weak probes must not accidentally retain the old version.
   Never run another LogAndApply during that unlocked transaction.

Gate/thread cleanup and pin reset happen under their existing synchronization;
avoid fatal assertions while a worker is blocked. Reuse existing OperationGate,
MemoryFileSystem and manifest parsers, not a new test framework.
No allocator-failure observer is claimed for snapshot/candidate/vector reserve.

Build modern_leveldb_unit_tests and modern_leveldb_windows_lock_child.
Coupled anchored selector:
`^(VersionSetTest|VersionBuilderTest|VersionTest|RecoveryTest|DatabaseTest)\.`
Measure original unique/nonempty class names and both additions; require final
exact identity equality with zero failures/errors/skips.
After required-field and prepared-state topics use `^VersionSetTest\.`.
Do not substitute older 119/121/123 baselines for measured new counts.

Independent source-backed design acceptance precedes cases; independent
original-case review precedes source topics; integrated code/evidence review
precedes publication. Require all exact reviewed-head checks and actual hosted
Windows Debug selected-name parity before normal merge/path equality/cleanup.
Docs-only checks do not certify runtime, hypothetical allocation/exception
counts, physical ownership copies or whole-file/production completion.

### Independent design review

The bounded read-only reviewer traced scoped source, private/public contracts
and existing characterization. No significant issue was found. Source-declared
five-class counts are 39/12/1/17/54, totaling 123; adding two cases would suggest
125, not measured runtime evidence. No edits/builds/native execution or nested
agents were performed. Implementation, allocation-failure observations,
exact-head CI and actual hosted identities remain unverified.
