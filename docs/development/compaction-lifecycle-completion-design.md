# Compaction Selection and Execution Lifecycle Completion

## Scope

Hardest-first completion batch for engine/compaction.cc/.h and
compaction_picker.cc/.h.
Source baseline: `ba6ecf276a630aaee68ff5d14c98ef0751d881f8`.
Prior C01-C04 delivery #194 and C05 delivery #196 remain accepted evidence, not
whole-file exemptions.

The current bounded inventory contains 36 named definitions and two picker
lambdas, including the two deleted NewCompactionIterator rvalue overloads in
the header. This batch targets the remaining initial-candidate/input-closure
orchestration, compaction iterator source assembly, and complete input-run loop.
No on-disk format, selection priority, overlap/drop policy, output transaction,
snapshot semantics, hook order, cache policy, mutex policy, benchmark,
diagnostic or coverage policy change.

## Picker completion

Add InitialCompaction containing selected level and source files. Add
SelectInitialCompaction(version, comparator, compact_pointers, seek), preserving
size score priority over seek pressure, compact-pointer first-file-after/wrap
policy, and null when neither source exists. The existing compact-pointer
find_if predicate moves unchanged into this selected child.

Add CloseLevelZeroInputs when the selected level is zero, computing the initial
range and transitive level-0 overlap closure. Other levels retain the selected
single source.

SetupInputs keeps boundary-file closure and initial two-level range. Add
ApplyExpandedInputs, which invokes the delivered TryExpandInputs and, only on
success, replaces both owners/range then recomputes the combined range.
Add CollectGrandparents, returning level+2 overlaps only when that level exists.
SetupInputs composes these stages and final compact-pointer/Compaction ownership.

PickCompaction becomes initial selection, optional level-0 closure, then
SetupInputs. All files remain owned by the retained Version.

## Execution completion

Add AppendCompactionInputs, iterating both input levels in order and delegating
to AppendLevelIterators with fill_cache false. NewCompactionIterator owns the
children vector and constructs the merging iterator.

Add RunInputEntries, preserving exact order: SeekToFirst, before-entry hook,
grandparent advancement/output boundary, key parse, history/drop decision,
optional output append, then input Next. It returns the first hook/output/input
or parse-corruption error and never calls FinishCompaction.

RunCompaction retains comparator/state owners, invokes RunInputEntries, then
returns any failed loop Status immediately; only complete success invokes
FinishCompaction. Outputs, VisibleHistory, BaseLevel and GrandparentOverlap keep
their previously delivered responsibilities and lifetimes.

## Selected ten-definition ledger

| Definition | Responsibility |
| --- | --- |
| SelectInitialCompaction | New size-first/seek-second initial candidate owner |
| CloseLevelZeroInputs | New transitive L0 closure stage |
| ApplyExpandedInputs | New optional expanded owner/range replacement |
| CollectGrandparents | New level+2 overlap collection |
| SetupInputs | Boundary/next-level/expansion/grandparent/final owner parent |
| PickCompaction | Initial selection, L0 closure and SetupInputs parent |
| AppendCompactionInputs | New ordered input-level iterator assembly |
| NewCompactionIterator | Child owner and merging iterator construction |
| RunInputEntries | New complete input seek/process/advance loop |
| RunCompaction | State-owner construction, input run and finalization |

## Thirty-two inspected unchanged definitions

The remaining 30 definitions retain their existing bounded responsibilities:
MaxBytesForLevel, TotalFileSize, Widen, RangeOf, OverlappingFiles,
AddBoundaryInputs, TryExpandInputs, ScoreCompaction, IsTrivialMove,
CompactionEdit; GrandparentOverlap constructor/ShouldStopBefore/AdvancePast;
BaseLevel constructor/IsBaseLevelForKey/LevelContains; Outputs constructor,
open, finished, Add, Finish, Open, Append, FinishBuilder, MakeMetadata;
VisibleHistory constructor, ShouldDrop, StartUserKey; ProcessEntry and
FinishCompaction; and the two deleted NewCompactionIterator overloads rejecting
temporary Compaction or comparator owners.

The AddBoundaryInputs partition predicate remains unchanged. The compact-pointer
search predicate moves unchanged into selected SelectInitialCompaction. Final
implementation must reconcile 42 named definitions and two lambdas.

## Characterization and delivery

Before production edits, add public/internal compaction cases proving:

1. Size pressure still wins over a simultaneous seek candidate, compact-pointer
   selection wraps after the last file, and level-0 closure remains transitive.
2. NewCompactionIterator uses identical internal keys with distinct values
   across multiple level-0 files and the next-level run, proving forward and
   reverse child precedence. A nonzero-level earlier-child failure proves the
   source run precedes the next-level run and later children are not opened
   after the first error.
3. The input loop invokes hooks/overlap/drop/output/Next in exact order and does
   not finalize after an input or hook error. An open-output malformed-key case
   must also prove no output Finish, cache verification or directory sync occurs
   after parse corruption.

Fresh-build compaction, picker, iterator, table-builder, lookup and Database
suites on unchanged production; freeze exact identities and independently review
the cases. Use small picker, iterator-assembly and input-loop production commits,
then integrated review of every definition and both lambdas.

One non-draft PR must pass exact-head hosted checks, GCC 13 branch coverage and
actual Windows Debug original-name parity before normal merge. This design and implementation are independently reviewed. Delivery remains
pending and this batch does not certify engine/platform/API completion.

## Independent design review

Independent review corrected the definition count, lambda migration, complete
loop-error contract and discriminating iterator-order characterization above.
No production edits or tests were performed during this design review.

## Local implementation evidence

Original-source characterization commit `b9c767f` strengthens malformed-key
finalization suppression. The unchanged/final selected compaction/picker/
iterator/table-builder/lookup/database set contains exactly 137 identities.

Production topics are `1df6b3a` (input-source assembly and complete execution
loop), `fcad7e2` (initial size/seek selection and level-0 closure), and
`cf28535` (optional expansion owner replacement and grandparent collection).
Final identities exactly match original.

The mechanical scanner reports 40 definitions and two lambdas because it still
classifies the two deleted header overloads as declarations; the complete manual
ledger is 42 definitions and two lambdas. GCC Release Werror compiles picker and
both changed test objects; compaction.cc remains blocked by the documented
pre-existing GCC16 optional-vector `maybe-uninitialized` diagnostic that also
occurs on baseline. Hosted GCC13 remains authoritative.

Independent integrated review found no significant issue.
