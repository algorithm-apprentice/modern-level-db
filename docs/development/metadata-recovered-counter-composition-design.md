# Recovered Metadata Counter Composition

## Status and scope

Proposed recursive follow-up to the
[production composition plan](production-function-composition-plan.md).
Source-read baseline: `748bf01`, after diagnostic dispatch delivery #236.

Scope is six definitions within the existing private ManifestRecovery in
`src/metadata/version_set.cc`, existing VersionSet public-operation tests and
this document. No header, replay/decoder/builder implementation, CURRENT,
prepared write/publication, snapshot, retention, allocation policy, format,
benchmark or coverage change. ValidateCounters remains a separate recursive
frontier; this batch does not certify the whole private recovery struct.

## Ordered accumulator responsibilities

Keep ApplyRecord's existing order: checked decode, comparator check, checked
builder Apply, compact-pointer updates, then AccumulateCounters. Its const
decoded Result continues owning the VersionEdit throughout all nested calls.
Do not move the edit or introduce a recovery-state/result envelope.

AccumulateCounters first calls IncludeAddedFileNumbers with a borrowed edit.
That child retains the original ordered new_files traversal and each
largest_number = max(largest_number, added.file.number). Deleted historical
files still constrain the maximum; do not replace history with final topology.

Next call AccumulateLogNumber(edit, offset), retaining the original optional
presence check, decreasing-log comparison, exact InManifest corruption and
assignment only after successful validation. Keep the original getter
expressions inside this child rather than hoisting or caching values.
Its successful empty Status permits the next step; a mutable parent Status
returns failure by implicit move, not a new const-Error copy.
The new status boundary is not a claim of identical physical elision.

AccumulateSparseCounters keeps next-file and last-sequence presence checks and
assignments, then previous-log value_or(previous_log), in original order.
Absent fields carry forward; explicit previous-log zero resets it. Next-file
and sequence are last-present, not monotonic; do not silently validate them
against earlier values or require a field in every record.
Finally the parent retains the original max of accumulated history, current
log and current previous log through the existing arithmetic primitive.
An explicit previous-log reset cannot remove an earlier historical maximum.

On a decreasing-log failure, added-file maxima and preceding compact-pointer/
builder updates remain in the still-private failed replay state, as originally.
Do not roll them back, publish that partial set, read another event, or replace
the record-offset error with an EOF consistency/build error.

## Final counter publication boundary

Finish retains its mutable validation Status and checked Version Result.
Required-counter validation still precedes checked Build and its exact
`"MANIFEST: "` error translation. Only both successes permit the new
PublishRecoveredCounters(set, selected_number) child.

This child retains exactly the original five assignments: manifest number
max(next_file.value(), selected_number + 1), next allocation from that number,
last sequence, log number, previous log number. Do not change reserved-number
arithmetic, turn sequence/next into historical maxima or add policy guards.
Do not mark a newly extracted function noexcept without a separate requirement.

After the child returns, Finish still makes the shared Version owner and calls
existing Install. Keep that allocation **after** the counter assignments;
moving it earlier would change original exception/evaluation order.
The parent checked Version Result remains alive through publication and owner
construction. The VersionSet/builder/reader owner scopes in Recover are unchanged.
The partial set remains unreturned until all finalization succeeds.
Counter extraction is not a new atomic transaction or allocation guarantee.

## Six-definition recursive ledger

| Definition | Responsibility and children/stopping reason |
|---|---|
| `AccumulateCounters` | Compose added-file history, checked log progress, sparse last-present fields and existing final max. |
| `IncludeAddedFileNumbers` | Fold each ordered added number into the historical maximum using existing max. |
| `AccumulateLogNumber` | Apply the single optional log progress policy with its original offset error. |
| `AccumulateSparseCounters` | Apply ordered last-present next/sequence and carry-forward/reset previous-log fields. |
| `Finish` | Compose existing validation, checked Build, recovered counter publication and owned topology installation. |
| `PublishRecoveredCounters` | Publish the validated replay's counter bundle in original assignment order. |

Existing max, value_or, optional assignment, Build and Install are named
primitives, not reasons for renaming wrappers or further owned staging objects.
The new children borrow; no header API or fields are added.

## Characterization before extraction

Retain existing sparse last-present/pointer cases, deleted-file historical
maximum, record-fault priority, required-counter-before-overlap-build, repaired
manifest numbering, lock/persistence and database recovery integration.
Add two public-operation cases on unchanged production:

1. Earlier previous-log history constrains a later reduced next-file even after
   an explicit previous-log reset to zero. Require the exact historical-maximum
   error. A corresponding valid bound keeps the latest decreased sequence,
   carried log and reset previous log through an empty record; inspect exact
   manifest/next reservation and no recovery writes, not producer-derived state.
2. A syntactically valid decreasing-log record also leaves next-file absent and
   would produce overlapping final topology. Require the exact offset-wrapped
   `"log number decreases"` before EOF required-counter/build errors. Provide
   another physical block and arm its read failure; require only one MANIFEST
   block read and unchanged input bytes. This does not observe private-state
   mutation order or prove allocator/destructor counts.

Use the established anchored coupled native Windows selector:
`^(VersionSetTest|VersionBuilderTest|VersionTest|RecoveryTest|DatabaseTest)\.`
Build both the unit executable and required windows-lock child target.
Measure unique original identities after additions; require every addition and
no failure/error/skip. Run VersionSetTest between the two source topics, and
compare final coupled identities exactly with the original XML.
Do not reuse older CURRENT/snapshot/diagnostic totals as this new baseline.

## Review and delivery

Independently review and accept this design before characterization.
Independently review original-source cases before production edits.
Use two small topics: ordered accumulator children, then final counter bundle
publication. One non-draft implementation PR may contain both.
Combined independent review checks the accepted-baseline diff, real earlier
decoder/builder/reader failure paths, getter order, borrowed edit/Version/set
lifetimes, mutable error moving, private partial-state behavior and allocation
position. Record actual proof and inspection limits honestly.
Require all exact reviewed-head checks, actual hosted Windows Debug selected
name parity, normal merge and byte-exact reviewed paths in main.
Docs-only acknowledgements are not runtime evidence or a final production audit.

## Independent design review

Bounded read-only review found no significant issues. It inspected the plan
against recovery, getters/decoder, checked builder, WAL framing/reader,
installation, affected existing tests and Windows target wiring. Distinct-file
overlap is deferred to Build, and small full records in a complete first block
can reach the proposed log check without reading a later physical block.
No edits, builds, tests or further delegation were performed. Implementation,
actual case identities/counts, native execution and allocator/destructor
behavior remain unverified; these source conclusions are not runtime evidence.
