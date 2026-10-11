# Lookup source decisions and lazy candidate traversal

## Status and scope

Design proposal for Q01/Q02/Q03 from the
[production composition plan](production-function-composition-plan.md).
No production implementation is included in this design PR.

Scope is `src/engine/lookup.cc` source decisions and candidate visitation, plus
sharing the existing owned-value copy with `src/table/table.cc` through a small
private `src/base/bytes_internal.h`. Public APIs, cache admission, metadata
topology, comparator policy, seek-budget mutation, iterators and benchmarks are
unchanged. The new header is private, not installed public API.

The API preparation implementation #188 changes none of these source/header/test
paths. This design adds only its own document, not the shared ledger, and has no
scheduling dependency on #188. Implementation requires this design's reviewed
merge and characterization on unchanged production.

## Original contracts

LookupValue resets seek at entry, then searches mutable memory, optional
immutable memory, overlapping L0 tables newest first, and at most one table
per deeper level. A visible value or deletion decides immediately; Missing
continues to older sources. False deletion and absent decision are distinct.
Value overwrites output with an owned copy; non-value/error paths leave it
unchanged. All captured sources remain pinned by the caller.

ForEachOverlapping reserves space for all L0 files, filters by inclusive user-key
range under the configured comparator, and sorts matching pointers by descending
file number before the first visitor. Preserve reserve size, comparisons,
short-circuit lower/upper range tests, projection and sorting policy.
Do not instead copy shared metadata owners or sort the immutable topology.

Deeper selection uses the existing LevelCandidate internal-key partition point
and smallest-user-key check. It happens only after earlier visitors continued,
not up front. A value, deletion, visitor error or second sampling overlap stops
before later-level comparisons. Preserve configured comparator ordering and
internal sequence boundaries, not bytewise approximations.

A point read reaching a second file charges the first before searching the
second, including second-file errors. The visitor updates previous in its
current order. SampleCharge does no table I/O and stops after its second match;
it returns the first original version-file address only when there are two.
Charges borrow metadata references and do not retain file/version ownership.

## Q01/Q02: one lazy candidate sequence

Extract `UserKeyInFileRange` as one inclusive range predicate using the original
short-circuit configured user comparisons.
Extract `LevelZeroCandidates` to compose original reserve, range selection,
pointer collection and descending-number ordering.
Reuse LevelCandidate unchanged for deeper levels.

Introduce a file-local aggregate candidate cursor, not a generic iterator
framework or allocated object. Its state is only:

- Borrowed version, comparator, user/internal-key views.
- Owned ordered vector of pointers to the original `Version::File` objects.
- Monotonic L0 position and next deeper-level number.

Initialize this aggregate in its own named declaration in ForEachOverlapping.
Build L0 candidates at the original eager boundary, but compute no deeper
candidate during initialization. Do not store `Version::File` copies inside the
cursor: a returned SeekCharge must point into the original version, not cursor
storage that is destroyed on return.

Named cursor children:

| Function | Responsibility |
|---|---|
| `NextLevelZero` | One ordered L0 advancement: bounds check, return original file pointer at level zero and advance the position. |
| `NextDeeperLevel` | Compose monotonic level scanning with existing LevelCandidate; skip absent levels and return the next matching original file pointer. |
| `Next` | Compose L0 advancement with deeper scanning only after L0 exhaustion; return empty after all levels. |

ForEachOverlapping becomes one candidate loop containing the original visitor
continue/stop/error handling. Retain static_assert and if constexpr distinction
between bool and Result<bool>. Do not normalize bool to Result<bool>, add an
impossible sampled-error branch, evaluate the visitor twice, or advance the
cursor before observing the current visitor outcome.

Result<bool> visitor failures retain moved errors. Bool visitors remain
infallible and false stops. Candidate temporaries need only live through one
visitor call; stored charges retain original file addresses, not temporary
Candidate addresses. Private cursor state is per operation, not shared mutable
sampling/global state.

## Q03: memory decisions and common owned copy

Extract `SearchMemTable` returning the existing private Decision. It composes
one Lookup, value/deletion classification and owned value copying; Missing
retains the original fall-through meaning.
Extract `SearchMemory` returning optional<bool> to compose mutable/immutable
searches in order. An engaged false is a tombstone decision and must stop;
nullopt means both missed and permits table search. Null immutable is skipped
exactly as today.

LookupValue retains seek.reset before SearchMemory and creates table candidate
state only after memory has no decision. Preserve its table visitor, error-copy
boundary and found/deletion/missing return behavior. Do not expose Decision,
introduce another result taxonomy or change table/seek policy.

Move the already existing CopyLookupValue resize-plus-ranges-copy operation
unchanged into private bytes_internal.h as an inline shared operation.
Use it from memory lookup and existing Table::LookupDataBlock; remove the old
file-local duplicate. No new copy implementation, vector assign, capacity
policy or returned borrowed value.

This one owned byte-copy job couples resize and copy. Its callers retain
nonaliasing memory/file/block owners until completion; standard allocation
exceptions propagate as before. Table::Get still holds its BlockReference
across LookupDataBlock and the shared copy, including zero-capacity caches and
mapped backing. The shared helper changes no cache/pin lifetime.

## Recursive boundaries and scope limits

Range admission and ordered cursor advancement are domain operations, not
one-statement wrappers. L0 selection, deeper scanning, source search and parent
visitation are compositions of named children/standard primitives.
The local cursor is necessary to unify visitation while retaining lazy deeper
selection and the two visitor return categories; do not add a general generator,
class hierarchy, coroutine, callback registry or extra allocation.

Every new/changed function, cursor method and the relocated shared header
definition receives an implementation-ledger row. Include the header in the
remaining-definition audit; moving a function out of a .cc is not completion
by disappearance from its scan.
Existing LevelCandidate partition predicate, SearchFile, table/parser/cache and
other production internals remain subsequent recursive-review frontiers.

## Characterization before extraction

Retain memory-before-version/sequence visibility, L0 newest-first, deeper
internal-key boundaries, reverse comparator, table errors/read options, seek
charges and both existing lazy-comparator sentinel cases.
Use public LookupValue/SampleCharge and table Get, not cursor/helper tests.

Strengthen exact searched-table ordering using existing filesystem operation
logs, not only the fixture's unordered set. Cover several missing overlapping
L0 files followed by a deeper value, plus a stopping deletion.
Keep logs test-local; do not add unsafe shared production traces.

Add/strengthen no-later-comparison evidence for an early table error as well as
a value/deletion, and for sampling whose second overlap is in a deeper level.
Place tracked later-level bounds where selecting them would increment the
existing comparator counter. Assert the first charge before a second-file error
and that failure leaves the supplied output sentinel unchanged.
Assert exact `charge.file == &version.files(level)[index]` after both point-read
and sampling return, not only matching file numbers. This distinguishes the
original version slot from a copied owner or destroyed cursor storage.

Characterize direct mutable/immutable empty value, deletion and Missing with a
prefilled output and preexisting seek. Prove every memory decision resets seek,
opens no table, and returns an owned value that survives memory-source release.
A tombstone must suppress an otherwise matching older table/immutable value.
Retain table owned-value-after-backing-release and non-value sentinel cases for
the shared copy.

Run LookupTest/SampleChargeTest/SeekStatisticsTest/TableTest before/after,
recording exact JUnit case-set parity and no failures/errors/skips.
Use existing warnings-as-errors build, formatting/documentation/whitespace
checks and exact-head authoritative remote CI. No coverage exclusions,
threshold changes, allocation-failure conversion or whole-file claims.

## Review and delivery

Independent design review precedes merge. Separate characterization, L0/cursor
visitation, memory-decision and shared-copy topic commits form one coherent PR.
Independent combined implementation review precedes publication; applicable
exact-head successful checks precede normal merge.
The production-wide recursive composition goal remains open after this batch.

Independent design review found no significant issue. Its nonblocking request
for exact original-slot address assertions is included in characterization.
The reviewer inspected source/tests without executing builds or tests.
