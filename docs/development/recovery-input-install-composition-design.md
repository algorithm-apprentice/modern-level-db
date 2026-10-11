# Recovery input inventory and recovered-state installation

## Status and bounded scope

Accepted design for R01/R02 from the
[production composition plan](production-function-composition-plan.md).
Design-only #191 was independently reviewed and merged after all 20 exact-head
checks succeeded. The separate implementation record follows below.

Scope is PrepareLogsForReplay and RecoverDatabase in `src/engine/recovery.cc`.
Extract directory-entry inventory, selected-log replay/final flush, and new
recovery-log installation into named private children. Keep existing
PrepareDirectory, OpenVersions, ValidateReferencedTableFiles,
MarkReplayLogNumbersUsed, CreateRecoveryLog, BuiltTables and LogReplay policies.
No public API, MANIFEST format, trusted batch insertion, replay corruption
policy, native filesystem abstraction or benchmark changes.

Lookup implementation #190 changes none of these source/test/design paths.
This design can be reviewed and delivered independently, without touching its
shared-ledger updates. Implementation requires this design's accepted merge and
characterization on unchanged recovery code.

## R01: inventory before validation and reservation

PrepareLogsForReplay currently:

1. Lists the directory, propagating a listing error.
2. Parses native file names, collecting table numbers and selected log numbers.
3. Validates every referenced table against the inventory.
4. Sorts selected logs oldest first.
5. Reserves all selected numbers before any replay-generated output.

Extract private `ReplayInputs` containing the original set of table numbers
and vector of log numbers. Extract `CollectReplayInputs` over a borrowed span of
directory paths and the version set's current/previous log numbers. It composes
existing ParseNativeFileName with exact original classification/admission.
Use a named local aggregate for the result; no new generic directory scanner,
callback, filesystem I/O or error category.

Preserve unknown-name skipping, recognized .ldb table admission, and log
admission `(number >= current || number == previous)`. Legacy .sst and
same-number log entries do not satisfy a missing .ldb reference. A zero previous
number has the same comparison behavior as today. Preserve directory encounter
order and duplicates in the log vector until the original sorting step; do not
unique it or silently ignore a number beyond FileNumberLimit. Table set
deduplication remains the original set operation.

PrepareLogsForReplay retains the directory-result owner until it returns;
inventory stores only numbers, not borrowed path/name views. Keep missing-table
validation before sorting/reserving log numbers, so mixed missing-table/invalid
log input reports the missing-table corruption first. Existing validation and
reservation helpers retain exact messages and failure behavior.

## R02: replay and install children

Extract `ReplayLogs(LogReplay&, span<const uint64_t>)` to compose ReplayLog in
the prepared order and Finish after every selected log succeeds. Return the
existing Result<SequenceNumber>. No selected logs still invokes Finish; an
error stops without later replay, final flush, new-WAL allocation or install.
Keep the original ReplayLog and Finish-result failure guards and Error copies
inside the helper. Return the successful sequence unchanged. Move a newly
propagated helper error at the parent boundary instead of adding another copy
or error-string reconstruction.

RecoverDatabase continues owning VersionEdit, BuiltTables and LogReplay in
that original declaration order, keeping the replay's current memtable and
cache-eviction guard alive throughout installation. After successful ReplayLogs,
it retains `SetLastSequence(max(manifest_sequence, replay_sequence))` before
allocating the new WAL number. Do not fold authoritative MANIFEST and WAL
physical corruption policies together.

Extract `InstallRecoveryLog` to compose:

1. Allocate the original new file number.
2. Call existing CreateRecoveryLog with sync_wal_creation unchanged.
3. Set the edit's current log number and previous log number zero.
4. Call existing checked LogAndApply with the moved edit.
5. Return private named `RecoveryLog` aggregate with its owned WalWriter and
   exact allocated number.

The private result is `Result<RecoveryLog>`; retain exact error codes/messages,
original CreateRecoveryLog move boundary and LogAndApply Error copy. Parent
propagation moves the new envelope. No extra allocation, counter getter
substitute, fallback log number or optional owner is needed.

The new WAL is opened/synced, and its namespace barrier completes when enabled,
before the MANIFEST names it. sync_wal_creation false skips only those existing
WAL creation barriers, not table/MANIFEST durability. Do not set edit log fields
before CreateRecoveryLog succeeds or skip LogAndApply for empty replay.

RecoverDatabase calls BuiltTables::Keep only after installation succeeds, then
builds the same named RecoveredDatabase aggregate from the original lock/version
owners and the new private log result. It still releases its lock last on
failure and in the returned owner. LogReplay remains alive through install;
no move/destruction of its memtable or guard to make a helper signature easier.

On install failure, the helper's new-WAL owner is destroyed before returning
the error; originally it was destroyed before the parent's replay/guard/version
and lock. Preserve that relative cleanup ordering, including exception unwind.
A moved successful log owner stays alive through final result construction.
Unique-owner/aggregate moves do not require another throwing allocation.
Do not add explicit WalWriter::Close: its existing destructor is defaulted and
owns its WritableFile, and file-close policy is not changed by this extraction.
Do not remove durable-but-uninstalled output files; existing cache eviction and
retry/recovery semantics remain authoritative.

## Recursive ledger and stopping boundaries

| Definition | Intended responsibility |
|---|---|
| `CollectReplayInputs` | Compose parsing with admitted table/log number inventory; owned numbers only. |
| `PrepareLogsForReplay` | Compose acquisition, inventory, reference validation, sorting and reservation. |
| `ReplayLogs` | Compose selected log replay and the original final flush/sequence result. |
| `InstallRecoveryLog` | Compose number allocation, existing WAL creation and checked edit installation. |
| `RecoverDatabase` | Compose preparation/lock/version acquisition, input preparation, replay, sequence update, installation and returned ownership. |

These are bounded domain compositions, not wrappers for single assignments.
New aggregates are private per-operation owners, not a general recovery
framework. LogReplay::ReplayLog/Apply/Flush, BuiltTables and lower-level
directory/version/WAL operations remain subsequent recursive frontiers.
Do not certify the entire recovery file or trusted insertion from this batch.
Update the detailed ledger with exact implemented definitions before review.

## Characterization before production edits

Existing recovery tests cover option rejection, parent durability, exact
table/log numbers, replay sequence/tombstones, write-buffer splits, physical
corruption/truncated tails, intact malformed batches, required log selection,
missing-table-before-invalid-log priority, cache eviction and retry after every
filesystem failure. Retain these and directly coupled VersionSet/WritePath
tests with the existing actual unit target.

Strengthen missing-table errors to exact original text and verify selected-log
open order, not just final entries/counters. A test-local MemoryFileSystem
subclass may reverse ListDirectory's successful result so sorted replay cannot
pass by relying on the normal fixture's ordered map enumeration. Keep native
file parsing and error paths unchanged; no production tracing or sorting in
the test override.

Cover a successful empty selected set that preserves the recovered sequence
and nevertheless installs a new empty WAL with previous log zero. Add mixed
unknown/legacy/nonselected filenames alongside selected logs, and assert all
selected numbers are reserved before the first table/new-WAL allocation.
Use actual output numbers and log-open operation order as independent oracles.

Retain the every-operation failure matrix's same-cache retry/entry assertions.
Strengthen exact error codes as well as messages, and ensure no later selected
log or final-install operation runs after a failed earlier replay.
Do not pretend MemoryFileSystem logs unlock/native handle destruction: its lock
destructor is unlogged and retry success proves release. Native resource
destruction and allocation injection are not newly measured by these tests.

Record exact unique JUnit before/after selected case-set parity, added names
present and no failures/errors/skips. Use warnings-as-errors build, existing
format/documentation/whitespace checks and authoritative exact-head remote
coverage. Do not adjust thresholds/exclusions to accommodate extraction.

## Review and delivery

Independent design review precedes accepted merge. Characterization, inventory
and replay/install are separate topic commits in one coherent implementation
PR. Independent combined implementation review precedes publication; all
applicable exact-head successful checks precede normal merge.
The production-wide recursive composition goal remains open.

Independent design review found no significant issue after inspecting recovery
source/tests and lower-level version/WAL contracts. It did not execute builds
or tests. The error-copy boundaries and original parent ownership lifetimes
remain explicit implementation-review requirements.

## Implementation record

Characterization was committed before production edits. Two new cases prove an
empty selected set still installs the exact new WAL/sequence and an earlier
replay failure suppresses later replay/output installation while allowing
same-cache retry. The existing required-log test now reverses the successful
directory listing and verifies exact selected sequential-open order plus original
output numbers; unknown, legacy and temporary names do not reserve numbers.
Mixed missing-table/invalid-log priority checks exact missing-table text.
Every-operation failure matrices now check error code as well as message.

The test-local listing override required its original const qualification;
that compile-only harness mistake was fixed before the recorded passing
original-production run and characterization commit. Actual selected test
names are RecoveryTest, VersionSetTest, InsertBatchTest and CommitGroupTest,
not a nonexistent WritePathTest suite.

Subsequent inventory and replay/install topic commits implement exactly the five
ledger definitions above. ReplayLogs retains both original error-copy guards;
new parent envelope propagation moves. InstallRecoveryLog retains the WAL/error
and checked-install boundaries and returns its named private RecoveryLog owner.
RecoverDatabase retains parent-owned replay/eviction guards, maximum sequence
update, Keep-after-install and the same named returned aggregate.

All 46 selected recovery/version/insertion/commit cases pass before/after with
exact unique JUnit case-set equality, both added names present and no
failures/errors/skips. The inventory slice passes all 17 RecoveryTest cases,
including both failure matrices. Each production slice rebuilt the actual
modern_leveldb_unit_tests target in Windows Debug.

The shared production ledger is intentionally not edited in this implementation
while #190 owns its pending updates. These R01/R02
delivery records can be incorporated by the next single ledger writer.
Remaining LogReplay, version/WAL/native ownership and allocation-injection
frontiers are not certified.

Independent combined implementation review found no significant production
correctness/design issue, but noted that selected logs 5/7/8 were below the
already recovered allocation counter, so output numbers alone did not prove
reservation. The required-log test now selects 5/7/20, explicitly orders their
opens, and expects table 21/new WAL 22. The strengthened test passed in all
46 cases against the original accepted-design recovery source and restored
reviewed extraction, with exact JUnit parity.

A bounded independent follow-up confirmed the high-number oracle resolves the
finding and independently inspected both 46-case XML sets. It did not execute
tests, rereview unchanged production, or independently prove source provenance;
the main agent verified the original-source/restored-source build sequence.
Authoritative exact-head remote CI remains the delivery gate.
