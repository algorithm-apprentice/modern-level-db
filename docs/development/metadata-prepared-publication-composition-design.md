# Metadata Required Fields and Prepared Publication

## Status and scope

Implemented recursive follow-up to the production composition plan, after
recovered-counter #238 and snapshot/liveness #225 delivery.
Source-read baseline: `6df11fc0edd02c915532a48269c5bd5c7de916cc`.
Design accepted through #247. Implementation starts from accepted main
`7e7b060b2e4bd169f8458cfe9ac052bb13e28c7b`, including independently delivered
key #245; that batch uses separate source/test/design paths and is not a
semantic prerequisite.

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

## Implemented topics and original-source evidence

The two original-source cases were committed as `82472a7` before production
edits. Both scoped production paths were byte-equal to accepted main.
The six-row required-field matrix first accepts files 2 and 3 individually,
then observes missing next/log/sequence, beyond-limit next, historical bound
and final overlapping topology in exact order and with exact messages.
Nonzero beyond-limit next goes through the actual setter/encoder/decoder;
no malformed sequence or direct counter injection is used.

The publication matrix runs successful and failed CURRENT temporary sync:
recovered MANIFEST 1 names file 2 and old level-2 pointer `a`; reserved new
MANIFEST 3 prepares deletion 2/addition 4, log 4, previous log 2 and duplicate
level-2 pointers `c`, then `d`. The gate is `sync 000003.dbtmp`, after durable
MANIFEST append/sync and before CURRENT rename. A weak old-version probe and
owned State observations cannot retain the old version accidentally.
During unlocked I/O the foreground obtains the old raw ReadPin, observes
unchanged pointers/topology, allocates number 5 and advances sequence 7 to 8.
The worker is opened/joined and the hook cleared before fatal assertions;
the pin is inspected and destroyed under the same external mutex.

On success, live files are 2/4 until pin release, then only 4, and the weak old
version expires. On failure, current stays at file 2 and the weak probe remains
live; the exact I/O error is sticky and a subsequent apply performs no I/O.
Both rows retain live sequence 8 and next allocation 6. Both written MANIFESTs
contain the complete original snapshot and complete captured edit (next 5,
sequence 7, both duplicate pointers), compared after decode/re-encode using
unchanged VersionEdit production code. CURRENT stays MANIFEST 1 on failed sync
and becomes MANIFEST 3 on success. These are public source-bound observations,
not independent wire-codec or general concurrent-writer certification.

The read-only independent original-case reviewer traced source, setter,
trusted builder, synchronization, snapshot, weak/raw ownership and MemoryFS
contracts plus supplied native evidence. No significant issue was found.
It performed no edits, builds, execution or nested delegation.

Three small production topics follow that characterization:

1. `f7d6bfb`: required fields before recovered bounds; existing mutable parent
   Status, const next owner, exact error priority and Finish owners remain.
2. `e8bb54e`: two real compact-pointer callers share the ordered assignment;
   parent-owned edit/array/snapshot/candidate compose counter fill, snapshot
   preparation and successful pointer/log/topology publication.
3. `3378dfc`: late old-current retention and weak registration/strong transfer
   compose InstallPrepared; erase_if and expiration lambda remain directly
   visible in the parent, and ReleaseRead remains unchanged.

Private declarations mirror their definitions. No public API, additional
staged array/snapshot/version owner, new input assertion/validation guard,
changed write/relock/failure policy or new annotation was introduced.
Pointer copying and snapshot materialization
retain their potential exceptions before I/O; publication has no added
noexcept contract. RetainPinnedCurrent/RegisterCurrentVersion preserve the
original reserved-capacity InstallPrepared noexcept invariant.

### Measured local native identities

Fresh Windows Debug unit/lock-child build and the anchored five-class selector
measured 125 unique identities both before and after production changes:
VersionSetTest 41, VersionBuilderTest 12, VersionTest 1, RecoveryTest 17 and
DatabaseTest 54. There are zero failures/errors/skips; both new public names
are present. Original and final `(classname,name)` sets match exactly.
After each of the first two production topics, VersionSetTest has exactly
the original 41-identity subset, not merely the same count.

Persistent original/final evidence is `metadata-prepared-before.xml/.log` and
`metadata-prepared-after.xml/.log`; intermediate evidence is
`metadata-prepared-fields.xml/.log` and `metadata-prepared-staging.xml/.log`.
Both source/header coverage/lint annotation full-text multisets are unchanged
against accepted main, not just annotation counts.

No allocator-failure injection, physical copy/elision claim, benchmark claim
or whole-file/every-definition completion is inferred from these checks.
GCC 13 authoritative diff-branch coverage, all reviewed-head CI checks and
actual hosted Windows Debug selected identities remain delivery gates.

### Independent integrated review

The read-only reviewer independently traced the four scoped paths against
accepted main through all three production topics and the evidence update.
No significant issue was found. Supplied original/final/intermediate XML/logs
support the exact measured identity claims; no edits, builds, execution or
nested agents were performed. Hosted CI, authoritative GCC 13 diff coverage,
allocator-failure behavior and every-definition completion were not certified.
