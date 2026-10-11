# Public database opening option preparation

## Status and scope

Design accepted in #187 for A01 in the
[production composition plan](production-function-composition-plan.md).
Design publication/merge precedes production implementation.

Scope is option admission/composition inside `Database::Open` in
`src/api/database.cc`, with related public API tests. No public header, engine
opening/recovery, filesystem policy, options mapping, read/write/snapshot
wrappers, or benchmark changes.

The checked-record implementation #186 touches none of these source/test paths.
This design adds only its own document, not the shared ledger; there is no
scheduling-only dependency on #186. Implementation depends on this design's
own independent review and merge.

## Existing order and ownership

Public opening currently performs:

1. Reject zero block_restart_interval with its existing InvalidArgument text.
2. Translate the public compression enum using EngineCompression; unknown
   values fail before Zstd-level validation.
3. Validate the translated compression options using ValidateCompressionOptions.
4. Convert admitted public options using EngineOptions.
5. Move engine options and directory into DatabaseEngine::Open.
6. Only after engine success, allocate shared DatabaseState with the retained
   comparator owner and engine, then construct the public database handle.

None and Snappy ignore the unused Zstd level. Zstd admits [-5, 22], inclusively.
Do not reorder validation, apply Zstd-only checks to other codecs, or remove
either admission boundary because a downstream operation also checks options.
Engine option sanitization and native filesystem admission remain unchanged.

Options keeps ownership of its shared comparator throughout preparation and
engine opening. EngineOptions contains only a borrowed comparator pointer,
choosing the existing BytewiseComparator fallback for null. Move the comparator
owner into shared state only after the engine successfully opens.
State destruction still closes the engine before releasing the comparator.

Open failure preserves its original error and returns no public handle.
Allocation/callback exceptions continue to propagate; no catch, retry, extra
state allocation, altered destructor ordering or success-shaped fallback.

## Intended decomposition

Extract private file-local
`PrepareEngineOptions(const Options&) -> Result<DatabaseEngineOptions>`.
It composes the original restart-interval guard, EngineCompression,
ValidateCompressionOptions and EngineOptions in their current order.

Retain the original one Error copy for failed enum translation and failed
compression validation. The parent uses a nonconst prepared Result and moves
its failed error through the new caller layer without another copy.
On success, move the contained engine options into the existing engine opener;
do not retain a reference into a destroyed temporary.

Database::Open becomes preparation, engine acquisition and shared owning state
construction. Engine acquisition errors still move directly to the public
result. Existing EngineCompression and EngineOptions are reused unchanged;
do not duplicate enum translation, option conversion or validate defaults.
Directory and comparator-owner moves remain at their original stages.

PrepareEngineOptions is a domain composition, not a generic validation runner.
Its children are already named operations; the remaining guard/error forwarding
belongs to admission. Do not add wrappers for a single assignment, a callable
pipeline/framework, new public validation API or extra ownership bundle.

The implementation ledger covers the new preparer and changed Open parent.
Existing mapping, enum representation, public-state/read-option helpers and
broader engine/native lifecycle functions remain separate recursive-review
frontiers. This bounded extraction does not certify the whole API file.

## Characterization before production edits

Retain native option rejection and comparator lifetime tests, Bloom options
readback, all compression-mode write/reopen cases, child-handle ownership and
portable option/default/aggregate compatibility.

Strengthen exact error text for zero restart interval, invalid public enum and
invalid Zstd level, retaining checks that invalid native openings do not create
their unique test directories. This proves observable admission behavior,
not a traced assertion that no filesystem method was called.

Add a portable public Open characterization with an unusable empty directory
and simultaneously invalid options. Prove restart failure precedes enum/level,
enum failure precedes Zstd-only level checks, and a valid Zstd enum reports its
level error before backend admission. Use default no-create behavior, not a
shared hard-coded database path.

Characterize successful Zstd at both admitted endpoints, including existing
negative-level evidence. Strengthen None/Snappy write/reopen cases with an
out-of-range unused Zstd level to prove it remains ignored. Public readback
proves successful operation and option admission; it is not a new independent
oracle for encoded compression type or every mapped engine option.

Comparator ownership remains covered by releasing the caller owner after Open,
using the live database, then destroying its last state owner and observing
comparator destruction. Do not mutate live immutable files, add production
tracing, expose private prepared options or inject allocation policy changes.

Run the existing PublicOptionsTest/PublicDatabaseTest selection before/after
and record exact JUnit case-set parity with no failures/errors/skips. Check
actual discovered portable/native names and fail empty selections.
Retain existing backend-disabled CI coverage where configured; local Windows
execution does not claim to execute that configuration.
Use existing warnings-as-errors builds, formatting/documentation/whitespace
checks and exact-head remote CI. No thresholds or exclusions change.

## Review and delivery

Independent design review precedes design merge. Separate characterization and
option-preparation commits form one bounded implementation PR; independent
combined implementation review precedes publication and all applicable
exact-head successful checks precede normal merge.

Record allocation-failure and platform evidence limits honestly. The production-
wide recursive composition goal remains open after this batch.

Independent bounded design review found no significant issues in this
preparation/Open composition and its validation/ownership/test contracts.
The reviewer did not execute tests or builds, verify remote CI, or certify
implementation, platform execution or whole-file completion.

## Implementation record

Implementation follows merged #187, with original-production characterization
committed before the option-preparation extraction.

| Function | Responsibility and stopping boundary |
|---|---|
| `PrepareEngineOptions` | Composes restart admission, existing enum translation, existing compression validation and existing option conversion, retaining original stage Error copies. |
| `Database::Open` | Composes preparation, engine acquisition and shared owning state construction; moves prepared errors/options without moving comparator ownership early. |

Existing converters, mappings, shared state layout, public signatures and native
engine lifecycle are unchanged. No coverage exclusions, thresholds, exception
handling, filesystem policy or allocation ownership changes.
The existing recursive frontiers above remain open.

Two new public cases and strengthened existing cases pass on original source
before extraction. They prove exact mixed-invalid priority against an empty
directory, exact native rejection text/no directory creation, both admitted
Zstd endpoints and out-of-range unused Zstd levels for None/Snappy.
Existing comparator/child-handle lifetimes and public readback stay covered,
without claiming readback verifies encoded compression type.

The actual `modern_leveldb_public_api_tests` target is rebuilt for both runs;
building `modern_leveldb_unit_tests` alone does not rebuild these cases.
All 16 selected PublicOptionsTest/PublicDatabaseTest cases pass before and
after with exact JUnit case-set parity, both new cases present and no failures,
errors or skips. Warnings-as-errors build and formatting, documentation and
whitespace checks pass. Independent combined implementation review found no
significant issues and separately confirmed the existing XML case sets without
executing tests. Local Windows execution does not certify the backend-disabled
configuration or allocation-failure behavior. Applicable exact published-head
CI remains required before merge.
