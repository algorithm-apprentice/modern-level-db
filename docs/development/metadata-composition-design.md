# Metadata Payload and Version Construction Composition

## Status and delivery boundary

Reviewed design proposal for M06 compound MANIFEST decoding and its paired
encoding frontier, M07 trusted-edit preparation, and M08 per-level
merging/shared ordering from the
[production composition plan](production-function-composition-plan.md).
The source baseline is `e9b5d6d5b35849f33034a7cef0bbeb71e7691d86`.

No implementation is included. This design is independent of #176's offline
dump refactor: it branches from main without those commits, its reviewed
metadata source/test context is identical, and this PR adds only this document.
Non-conflicting independent PRs may be published concurrently. Actual
code/interface dependencies still determine implementation and merge order.

Independent design review precedes publication; implementation starts only
after this design-only PR merges. One implementation PR can cover these related
metadata slices, with small internal commits and separate combined review.
When implementing, avoid concurrent edits to shared plan ledgers in other PRs.

Production scope: `src/metadata/version_edit.cc`, `version_edit.h`, and
`src/metadata/version.cc`, plus directly related characterization and docs.
Do not change VersionSet recovery/persistence, locks, native I/O, compaction,
benchmarks, public APIs, file-number policy, or trusted/checked admission rules.

## Source contracts and prior art

`VersionEdit::Decode` accepts unordered fields, keeps the last repeated scalar,
deduplicates/sorts deletions, and preserves pointer/addition insertion order.
`DecodeField` reads compound fields left-to-right using `ReadLevel`,
`ReadFileNumber`, `ReadNumber`, and `ReadInternalKey`, reporting the original
field-specific Corruption message. Zero is legal for log/previous-log numbers
and file size, but not next/deleted/new file numbers. All owned internal keys
exist before inserting a decoded entry. Keep new-file materialization separate
from `push_back`, including the existing GCC member-cleanup coverage rationale.

`VersionEdit::Encode` uses canonical comparator, log, previous-log, next,
sequence, pointer, deletion, addition order. Its tag numbers, varints,
length-prefixed fields, optional omission and container order are persistent
format contracts. Reuse `AppendTag`, `AppendNumberField`, and `AppendField`.

Checked `VersionBuilder::Apply` processes deletions in set order before
additions. It then validates/applies additions in insertion order, checking
global number liveness before key range within each entry. An earlier reversed
range can therefore win over a later live-number failure; do not replace this
with global validation passes ordered by error class.
`Build` sorts each level by smallest internal key, then file number, and rejects
overlap above L0. These failures remain InvalidArgument. Recovery and the
VersionSet overload without a supplied lock use this checked builder; the
runtime overload with a supplied lock uses `BuildTrusted`, whose edits already
satisfy its preconditions. Existing recovery wraps Apply/Build failures as
MANIFEST Corruption through `InManifest`; runtime builder failure propagation
and those recovery translations are not modified.

`BuildTrusted` counts all per-level deletions/additions, reserves all delta
vectors, collects deletions, then allocates all added metadata before merging
levels. Deletion numbers are already ordered per level by the edit's set.
Sort only the small added-file vectors; consume the already ordered base
forward. A deletion filters the old base entry, never an added entry with a
reused number. Untouched files keep shared-pointer identity and seek budgets.
New metadata copies edit fields and resets allowed seeks through the existing
`InitialAllowedSeeks`. Do not re-sort or rebuild the base, revalidate trusted
inputs, alter pre-I/O allocation boundaries, or mutate the base/edit.

`FileBefore` already implements the trusted ordering.
`InternalKeyComparator::Compare(const InternalKey&, const InternalKey&)`
directly delegates to encoded comparison, so checked sorting can share it.
No new comparator or general merge framework is needed.

## M06: compound MANIFEST payloads

Retain `Decode`'s checked tag loop, first failed field return, and scalar
decoding policy. Give `DecodeField` private children
`DecodeCompactPointer`, `DecodeDeletedFile`, and `DecodeNewFile`, declared in
`version_edit.h`. Each owns its field's temporary scalars/keys and composes the
existing checked readers, complete payload construction, and final insertion.
Switch dispatch returns their existing Status directly.

Move each current compound case without calling public mutators: those would
introduce InvalidArgument errors and duplicate policy. Preserve short-circuit
read order, error spelling/category, ownership, and insertion-after-complete-
decode. Do not return default payloads on failure, copy Error payloads, catch
allocation exceptions, or change cursor consumption.

Add paired file-local `AppendCompactPointer`, `AppendDeletedFile`, and
`AppendNewFile` payload encoders; each composes existing tag/scalar/byte
primitives in the original order. `Encode` coordinates its original optional
and container ordering and calls these children. Do not extract forwarding
wrappers around single primitive calls. The existing comparator and scalar
paths remain explicit compositions; deeper reader/encoder helper internals
remain subsequent per-function audit frontiers.

## M07: prepare trusted deltas and shared metadata

Use private prepared-delta state with the same separate per-level deletion
and addition arrays as today. It owns only temporary vectors/added pins for
this build, not base/comparator ownership, recovery state, or an install
reservation. Retain all-delta-reservation-before-population and all-added-
metadata-before-level-merge order. Counts can stay local to reservation.

Name compatible per-level counting, delta reservation, deletion collection,
and addition initialization as children below `PrepareChanges`. A small
file-local count-by-level operation can serve both record ranges because their
level/index/count contracts are identical; no callbacks or public utility.

Share `MakeVersionFile(const FileMetadata&)` between checked `Apply` and
trusted addition collection. It copies metadata, resets its seek budget via
`InitialAllowedSeeks`, and creates one immutable shared metadata owner.
Checked validation remains before this call. Keep one metadata copy and one
shared allocation per new file, existing error precedence, and allocation
exceptions. Retained base pins and their mutable seek budgets are not rebuilt.

## M08: per-level ordered merge

`BuildTrusted` composes prepared deltas with per-level construction.
Use a private level merge operation with explicit borrowed comparator,
ordered base, sorted deletion numbers, added files, and owned output.
Sort additions with existing `FileBefore`, then reserve output to the original
upper bound `base.size() + additions.size()` before appending.

Name appending the eligible base prefix before one addition, appending a
retained base file, and draining the remaining base as distinct children.
The cursor advances monotonically, each base pin is visited once, deletion
membership uses existing `std::ranges::binary_search`, and each new pin is
appended unconditionally after its prefix. Preserve comparator tie breaking,
duplicate-number reuse after deletion, output order, and original pin lifetimes.
Do not hide this algorithm in callback abstractions or add post-hoc validation.

Reuse `FileBefore` in checked `Build`'s sort lambda, keeping its overlap loop
and checked errors separate. Checked Apply/Build's remaining orchestration
is not certified by this small shared-operation substitution.

## Recursive responsibility boundaries

All new parents must record their actual named children in the implementation
ledger. Count-by-level is one histogram operation over valid record levels;
retaining a base file couples deletion membership with its conditional pin
append; metadata initialization couples owned copying, budget initialization,
and owner construction using existing primitives. Payload encoders/decoders
and preparation/merge operations are compositions, not exempt complex leaves.
Do not use function length as the stopping reason.

Existing readers, internal-key encoding/comparison, seek-budget arithmetic,
VersionSet orchestration, and other production functions remain later audit
frontiers. This batch does not certify both source files or the entire tree.

## Characterization before extraction

Existing VersionEdit tests cover exact canonical bytes, round trips/boundaries,
unordered/repeated fields, deduplication, every truncated prefix, malformed
tags, invalid levels/numbers/sequences/keys, and field-specific truncated errors.
Retain them; strengthen exact Corruption messages for the three compound
payloads across failures at each constituent reader, using existing builders
and malformed encodings. Both first and second new-file keys need malformed
and truncated cases. Require first failed field to win when a later unknown
tag follows a complete semantically invalid payload (bad level, zero number,
or invalid key kind). Do not append that tag to a truncated varint/key and
accidentally complete or alter the truncated payload. These unchanged cases
must pass before production edits.

Existing VersionBuilder tests cover checked/trusted equivalence, sorted files,
sharing, moves/reused numbers, large-base merging, and checked rejection.
Add a focused L0 merge characterization with equal smallest keys/file-number
ties, deleted-and-readded same-level numbers, empty deltas/levels, deletion-only
and addition-only edits, plus exact untouched-pointer identity and unchanged
base/edit content. Compare checked and trusted output for valid edits.

Characterize new seek budgets around the 100-seek minimum, at a higher budget
and maximum uint64 file size; retained pins keep a deliberately changed budget.
Explicitly include zero file size in canonical encode/decode round trips and
both builders' 100-seek initialization, rather than treating a maximum-size
boundary test as coverage of zero.
Do not claim the int64 cap is reachable: uint64 file size divided by 16384
remains below it. Both construction paths must agree without changing edit
metadata or the retained base.

Add exact-message mixed-failure checked-builder cases: a missing deletion wins
over invalid additions; a live-number failure wins over a reversed range in the
same entry; an earlier reversed-range entry wins over a later live-number
entry. Keep caller recovery wrapping separate from the builder's own error
category. These cases freeze actual phase/entry order, not a proposed global
hierarchy of error classes.

The existing large-base test verifies output, not complexity. Strengthen it
with a counting bytewise comparator whose counter is reset after base building.
Its counter must respect Comparator's concurrent-call contract, using relaxed
atomic accounting; separator/successor behavior delegates unchanged.
For a single new file, trusted construction must make at most
`base_file_count + 1` user-key comparisons: it sorts no base and its addition
sort needs no comparisons. Assert the count as well as exact output/pin sharing;
do not substitute elapsed time or a merely large input for this requirement.

Focused selector:
`^(VersionEditTest|VersionTest|VersionBuilderTest|VersionSetTest|DumpFileTest)\.`
Inspect actual discovered case names, record identical before/after JUnit
case sets without failure/error/skip, and run the warnings-as-errors build,
formatting and documentation checks. Slice runs may narrow to VersionEdit or
VersionBuilder; combined implementation review covers all changed code and
interactions. Require applicable green CI on the exact published head.

## Independent design review

The independent source-backed review identified two characterization gaps:
mixed-failure ordering could be mistaken for global error-class precedence,
and maximum file-size coverage did not establish zero-size behavior. The
requirements above explicitly preserve per-entry validation order, add exact
mixed-failure errors, distinguish recovery wrapping, and add zero-size format
and seek-budget cases. Review was static; no implementation, execution,
allocation guarantee, whole-file certification, or GitHub approval is claimed.

Before concurrent publication, the main agent verified that the metadata
source, dependent comparator/caller headers and files, and metadata test context
match between main and #176's reviewed head. This document does not change
#176's shared production-plan ledger or other paths.
