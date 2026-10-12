# Metadata Snapshot and Live-File Composition

## Status and scope

Design accepted in #224. This is a recursive follow-up to
[MANIFEST transactions](manifest-transaction-composition-design.md).
The source-read baseline is `2c6de1c`; the CURRENT/runtime batch changes
other definitions in the same source file and is not part of this slice.
Implementation starts only after that batch is delivered and this design is
independently reviewed and accepted.

Scope: file-local `Snapshot`, `VersionSet::LiveFiles` and their children in
`src/metadata/version_set.cc`, characterization in the existing metadata tests,
and this document. No public/private header, topology publication, read-pin
protocol, recovery policy, filesystem behavior or benchmark change.
`ApplyPrepared`, `InstallPrepared`, `ReleaseRead` and MANIFEST recovered-counter
composition remain separate recursive frontiers.

## Existing contracts

Snapshot first observes the comparator name and assigns it to a new edit.
For ascending levels, it appends a present compact pointer before visiting that
level's files in their stored order. `AddCompactPointer` and `AddFile` copy
their arguments and their statuses are deliberately discarded for these
internally valid values. Preserve that admission policy and allocation order:
do not group all pointers before all files, move metadata, pre-materialize
another container, cache all level spans, or add validation/error propagation.
The completed edit is encoded under the existing lock before unlocked I/O.

LiveFiles traverses installed weak versions in their existing vector order.
Each weak reference is locked once into a parent-owned shared pointer; expired
versions are skipped. That owner stays alive while every ascending level and
stored file is visited. The result is the existing sorted, duplicate-eliminating
set of file numbers. Do not retain extra versions, prune weak references,
change read pins, cache a raw pointer after dropping its owner, reserve another
container or mutate topology. Allocation failure still unwinds naturally.

## Composition

Snapshot keeps comparator assignment and the ascending level loop.
`AddPresentSnapshotPointer` receives the level's borrowed optional key, tests
presence and invokes the existing checked append primitive with its status
discarded. `AddSnapshotFiles` receives a borrowed file span and copies each
metadata value through the existing append primitive in order. Obtain that
span only after pointer admission, as before.

LiveFiles keeps result ownership, weak-lock/skip policy and the strong version
owner. `CollectVersionFileNumbers` traverses levels and delegates each borrowed
span to `CollectLevelFileNumbers`, which inserts numbers into the borrowed set.
The helpers introduce no reference-count increments, intermediate allocation,
callback abstraction or owning copies.

## Six-definition recursive ledger

| Definition | Job and stopping reason |
|---|---|
| `Snapshot` | Compose comparator observation and per-level pointer/file materialization in original order. |
| `AddPresentSnapshotPointer` | Admit one optional pointer; presence and copying that same key are one coupled operation. Existing `AddCompactPointer` is the child primitive. |
| `AddSnapshotFiles` | Materialize one level's ordered metadata through existing `AddFile`; no second policy in the traversal. |
| `VersionSet::LiveFiles` | Acquire each live installed version owner and compose file-number collection. |
| `CollectVersionFileNumbers` | Compose ascending per-level collection under a borrowed live version. |
| `CollectLevelFileNumbers` | Fold one file span into the existing ordered union; set insertion is the representation primitive. |

No wrapper merely renames `Name`, `SetComparatorName`, `files`, `lock` or set
insertion. Snapshot materialization and liveness union have different ownership,
allocation and result contracts; do not share them through a generic visitor.

## Characterization before extraction

Use public VersionSet operations and existing MemoryFileSystem/WalReader/
VersionEdit primitives, not a new production seam.

For snapshot, recover a topology containing empty and populated levels,
multiple files, and present/absent compact pointers, then trigger the first new
MANIFEST. Read its first logical record and compare exact encoded bytes with an
independently constructed expected edit. Verify decoded comparator, pointer
order and per-level file order, followed by the existing runtime edit. Existing
snapshot-before-unlock and failed-write/publication cases remain in the selector.
A reordered allocation attempt without changed bytes is inspection-based.

For liveness, use overlapping file numbers across multiple externally held
versions, empty/populated levels, an expired weak version and a raw read pin.
Check the exact union as external holders and multiple pins are released,
including a moved pin, then install again to exercise the existing pruning.
Retain no new owners in the test that would accidentally mask pin retirement.
Compare filesystem operation counts across LiveFiles calls to prove no I/O.
Owner lifetime through each traversal is additionally checked by inspection;
do not claim a deterministic concurrent-expiration experiment.

Run the coupled VersionSet, VersionBuilder, Version, Recovery and Database
selection on unchanged production first. Preserve the exact unique
classname/name set after two responsibility-sized source topics. Record counts
from actual XML, not from an older batch; no failures, errors or skips.

## Review and delivery

Independent read-only design review found no significant issue against the
unchanged baseline definitions, relevant headers, append semantics and existing
tests. Expected snapshot bytes must be constructed from explicit fixture
values, not actual topology; Encode groups fields and cannot prove allocation
interleaving. Release external holders before testing final-pin retirement.
Capture native baseline XML after characterization and before extraction.
No implementation, builds or tests were performed by that review.

Independent design review precedes design acceptance. Independent
characterization review precedes production extraction. Independent combined
implementation review checks actual baseline diff, borrowed-owner lifetimes,
per-level call/argument evaluation order and unchanged publication/pin policy.
One non-draft module implementation PR may contain the separate snapshot and
liveness source commits. Require reviewed exact-head hosted CI, including
unchanged coverage policy, before normal merge.

This six-definition slice is not a certificate for the whole source file or
the production-wide recursive audit.

## Original-production characterization

After CURRENT delivery #223, two public cases characterize exact snapshot
bytes/decoded field order and mixed holder/pin/expired-version unions. The
unchanged production/header baseline is `65ff295`. Snapshot expected values
come from explicit fixtures, not installed topology; tests compare the raw
first and second logical records before consuming the next record.
Pin retirement checks remove all external version holders first, retain only
weak observers and verify expiry plus exact unions at each release. LiveFiles
queries preserve the filesystem operation count.

Native Windows Debug original JUnit contains 121 unique classname/name pairs:
VersionSet 37, VersionBuilder 12, Version 1, Recovery 17 and Database 54.
Both new cases are present with no failures, errors or skips. Production and
header are byte-identical to baseline before this run. Allocation interleaving
and traversal-owner lifetime remain inspection-based.

Independent pre-extraction characterization review found no actionable issue
in either case and independently checked the 121-case XML. It confirmed no
hidden strong owner masks pin retirement and no WAL borrowed span is used
after advancing. The reviewer performed no edits/builds/tests.
