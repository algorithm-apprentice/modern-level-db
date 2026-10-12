# CURRENT I/O and Runtime Edit Validation Composition

## Status and bounded scope

Design proposal for recursive children left open by the MANIFEST transaction
batch #203. The inspected baseline is
e188a5f4dd3fbc54e3592d12f1c9617a3731080b.
ReadCurrent, InstallCurrent and runtime Validate are the selected parents in
`src/metadata/version_set.cc`; this is not another recovery/publication redesign.

Implementation may change that production file, its existing VersionSet unit
test file and this design. Use file-local helpers without modifying public or
private headers. Do not change MANIFEST replay, prepared allocation/unlock/relock,
trusted versus checked builders, Snapshot, retained-version topology, WAL/native
transport, benchmarks, diagnostics or coverage policy.

## Prior art and exact contracts

Reuse CurrentFileName, TempFileName, CurrentFileContents, ParseCurrentFileContents
and the existing FileSystem ownership APIs. OpenSelectedManifest already names
CURRENT selection/parsing and missing-MANIFEST translation; do not build another
filename/decoder stack. Runtime Validate is called before both the checked and
trusted apply paths, without changing their different builder contracts.

ReadCurrent opens its CURRENT sequential owner before constructing the
accumulated string. Each read requests exactly the same 64-byte buffer.
Preserve native-read error before oversized-count validation, then EOF before
append. Short reads are progress, not EOF; do not parse early or read ahead.
Read through EOF before ParseCurrentFileContents runs in the existing caller.
Malformed bytes plus a later read failure still report that read failure.
Keep the original read-error copy at its logical observation site; move errors
only through new envelopes. Sequential ownership remains alive through reading
and result construction and closes on the original return/unwind boundary.
No new CURRENT length limit, fallback/default filename or ignored read failure.

InstallCurrent owns its temporary writer. Compute the temporary path, open it,
then encode CurrentFileContents, append those exact LF-terminated bytes, Sync
and Close. Only afterward Rename to CurrentFileName and SyncDirectory.
The MANIFEST name barrier before this transaction remains in Write.
Do not precompute CURRENT bytes or snapshot a manifest number before OpenWritable:
preserve the existing allocation/observation order at the I/O boundary.
Each failed operation suppresses later transaction stages; retain its exact error,
original returned-Status copy sites and original scoped cleanup.
Do not remove the temporary file, add a second explicit close, retry a transaction,
or turn a failed name barrier into success. Existing outer sticky-error and
durable-before-publication policies remain unchanged.

Runtime Validate's priority is:

1. Next file number is exhausted only when greater than FileNumberLimit.
2. A present comparator name must match; an absent name does not call Name.
3. Effective current log defaults to log_number_, cannot move backwards and
   must remain strictly below the current next file number.
4. Absent previous log validates as zero, not prev_log_number_; a present
   previous log must remain strictly below the next file number.
5. Check each added file number in original edit order; report the first one
   not strictly below the next file number.

Preserve exact messages, boundary operators, defaulting and short circuits.
Do not merge recovery counter proof with this runtime InvalidArgument policy.
Read required scalar fields at each existing stage rather than eagerly caching
all counters or evaluating comparator Name before earlier guards.
Validation errors still perform no filesystem operations, leave installed state
unchanged and do not latch the write-failure state. Keep the public allocation
and externally synchronized caller contracts.

## Recursive definition ledger

| Definition | Composition or leaf stopping reason |
|---|---|
| ReadCurrent | Acquire the existing sequential owner, then read its contents; retains owner through child result/unwind. |
| ReadCurrentContents | One accumulated-byte progress operation: checked chunk, EOF decision and standard string append, over the unchanged 64-byte scratch buffer. |
| ReadCurrentChunk | One checked read boundary: original error copy or size-bound proof; no parse/append/EOF policy. |
| VersionSet::InstallCurrent | Existing path/open, complete temporary-byte persistence, rename and directory barrier; owns the borrowed child's writer throughout. |
| WriteCurrentTemporary | Compose existing encoding, Append, Sync and Close in the original order; borrowed writer, no acquisition or rename. |
| VersionSet::Validate | Compose the five ordered runtime proofs below; no builder, counter mutation, filesystem operation or sticky-error change. |
| CheckFileNumberCapacity | One next-counter exhaustion decision with its existing bound/message. |
| CheckComparatorName | One optional comparator-name proof; call Name only for a present edit field after earlier guards succeed. |
| CheckCurrentLogNumber | One effective current-log interval proof, retaining the original default, lower bound and upper bound. |
| CheckPreviousLogNumber | One zero-defaulted previous-log upper-bound proof, deliberately separate from application defaulting. |
| CheckAddedFileNumbers | One ordered per-added-file allocation proof; stop at the first invalid number, without sorting or invoking the version builder. |

Names may adapt to local terminology. Use concrete borrowed file/edit/comparator
references and necessary scalar bounds, not callbacks, parameter bags or a
general validator framework. Existing native/file/standard operations already
serve as children; do not wrap a single Append, Sync or Rename just to name it.
New propagation envelopes explicitly move errors where needed; do not place
Status/Result in boolean chains or conditional expressions.
Original const-Status return sites stay at their logical child locations.

## Characterization before production edits

Existing tests cover every new-MANIFEST failure, sticky behavior, CURRENT
malformation/oversized reads, invalid edits without I/O, and allocation bounds.
Retain them and strengthen missing exact boundaries on unchanged production:

1. Script CURRENT short chunks ending at EOF. Record identical 64-byte requests,
   exact concatenation/selected MANIFEST and owner lifetime through completion.
   Include malformed multi-chunk contents that are parsed only after EOF.
2. After partial CURRENT progress, inject read failure and oversized count.
   Assert exact primary errors, no later read or selected-MANIFEST open, and
   original owner cleanup. Do not expose a span for an oversized result.
3. Record the temporary-CURRENT stage suffix and exact bytes for success and
   open/append/sync/close/rename/final-directory-sync failure.
   Assert no later transaction phase, old-versus-new selected name at the rename
   boundary and no remove operation. Preserve original unrelated RAII cleanup
   events rather than declaring that failure performs no cleanup I/O.
4. Mixed-invalid runtime edits prove exhaustion/name/current-log/previous-log/
   added-file priority, strict equality bounds and first-invalid added-file order.
   Track comparator Name calls without changing its comparison semantics.
   Exercise absent fields, older current log and valid boundary values.
   Assert exact error text, no filesystem work, stable installed state and no
   failure latch; where counters remain valid, a later valid edit still succeeds.

Use test-local MemoryFileSystem/SequentialFile wrappers and existing failure
injection. Do not modify production seams, set private state or invent a red
test for unchanged behavior. Public MarkFileNumberUsed and NewFileNumber already
allow reaching the exhaustion boundary under the existing contract.
Owner cleanup observations in noexcept destructors must not allocate.
Allocation throws remain inspection-based unless existing supported tooling
can inject them without changing the production interface.

## Small topics and delivery

Independently review/accept this design before implementation. Add reviewed
original-source characterization commits and record native Debug JUnit evidence.
Then implement CURRENT reading, runtime proof and temporary-CURRENT persistence
as separate responsibility-sized commits for one coherent implementation PR.
Run VersionSet cases after each topic and combined VersionSet/Recovery/Database
cases after all three; use actual discovered suite names and reject empty runs.

Require exact original/final unique classname/name set equality, all required
new names, no failures/errors/skips, existing formatting/documentation checks,
and independent combined review before pushing.
Authoritative GCC 13 diff coverage and all other exact reviewed-head hosted
checks must pass, with no new exclusions. Preserve error copies, aggregate
initialization/declaration and other existing coverage-compatible conventions.
Normal merge only; verify all reviewed changed paths persist in main.
Detailed ledgers and delivered worklist rows do not certify unchanged helpers
or complete the every-definition production audit.

## Independent design review

Bounded read-only review found no significant issue in the complete proposal
and its relevant original implementation/tests. It confirmed meaningful child
responsibilities, exact observation/ownership/error-copy boundaries and feasible
unchanged-production characterization without private-state access.
Source/header/tests remain unchanged; the reviewer did not edit files, execute
builds/tests or perform allocation-fault experiments. This is design evidence,
not implementation runtime verification.
