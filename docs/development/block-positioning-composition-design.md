# Block checkpoint positioning and entry validation

## Scope

Design proposal for I03/I04 in the
[production composition plan](production-function-composition-plan.md).
Scope is block.cc/h and directly related characterization tests. No builder,
encoded format, comparator policy, table I/O/cache, public interface, benchmark
or coverage changes. This batch is independent of engine iterator production
paths and avoids the shared ledger while another implementation owns it.

## Checkpoint positioning

Seek keeps BeginPositioning, internal-target length validation, empty restart
handling, current-key comparison and search-bound narrowing in original order.
An equal current key still returns immediately without decoding restarts.

`FindRestart(target, left, right)` implements the original upper-midpoint
binary search for the last restart key strictly before target, using RestartKey
and Compare. Return Result<size_t>, moving new caller error envelopes while
retaining the original const Status error-copy site inside the child.
No sortedness revalidation or global scan.

`RestoreCheckpoint(index, current_key_compare)` preserves the exact reuse rule:
reuse only when index == restart_index_ and current key is strictly before
target; otherwise call SeekToRestartPoint and preserve its original failure
copy. `ScanToLowerBound(target)` parses forward before checking failure,
validity and comparison in the original short-circuit order.
Seek composes selection, restore/reuse and scan through those children.
Do not turn the current-position optimization into a fresh restart decode.
Prev/SeekToLast reconstruction loops remain separate recursive frontiers.

## Shared physical entry view

Keep existing bounded DecodeVarint32 and DecodeEntry logic unchanged, including
fast three-byte lengths, accepted extended encodings and payload bounds.
Move DecodedEntry and DecodeEntry into Block's private implementation interface
so named Block/Iterator children can consume the same checked representation
without a public type, new internal file or duplicated decoder.
DecodedEntry adds the decoded header byte count to shared/key_delta/value.
This count is a bounded pointer difference from the same original input, not
another parse or allocation. Existing RestartKey continues using this decoder.

`ReadEntry(layout, offset, previous_key_size)` calls DecodeEntry with the
original entry-region end and rejects malformed encoding or excessive shared
prefix. Return optional<DecodedEntry>; empty denotes malformed external bytes,
with the caller retaining its distinct error policy. It does not call
Corruption, validate internal-key size, reconstruct a key or invoke visitors.
Callers prove offset is inside the entry region as originally.
Compute entry end from offset + header_size + suffix/value lengths only after
the iterator's original internal-key size check.

`ReconstructKey(key, entry)` shares exactly resize(shared) followed by suffix
append between physical validation and iterator publication. It may allocate;
do not declare noexcept or reserve in the physical-validation path.
The iterator's existing reserve must occur before publishing current_.

## Physical validation and iterator publication

ValidateEntries keeps initial restart-count/zero-point validation and owned key/
restart_index/offset loop state in the parent.
`ValidateRestartEntry(layout, offset, shared, restart_index)` checks a pending
restart against the current entry and advances restart_index on exact match.
It preserves restart < offset and restart == offset with nonzero shared
rejection before key reconstruction and visitor execution.
The parent calls ReadEntry, restart validation, ReconstructKey and visitor in
that order, advances offset only after visitor success, and checks final matched
restart count with the existing empty-block special case.
Do not compare physical key order or silently skip duplicate/trailing restarts.

ParseNextEntry retains exhaustion invalidation, ReadEntry, internal-key length
check and bounded end calculation in the parent.
`CommitEntry(entry, key_size, entry_end)` reserves the key, publishes current_,
reconstructs key, then assigns value and next_ in original order.
It calls `AdvanceRestartIndex` only after this publication.
That child preserves the original restart bound check and strict
restart < current_ advancement; a restart exactly at current_ is not advanced.
On malformed later restart it uses existing Corruption, invalidating the
partially published state. No rollback, new error or eager restart audit.

Original Status/Error copy sites remain in children; newly introduced parent
envelopes move. Borrowed suffix/value spans retain block backing ownership.
Iterator key remains owned; moved Block storage still outlives its iterators.

## Definition responsibility ledger

| Definition | Domain operation or composition |
|---|---|
| `FindRestart` | Bounded checkpoint binary search under the borrowed comparator. |
| `RestoreCheckpoint` | Restore/reuse decision preserving optimized current state. |
| `ScanToLowerBound` | Forward reconstruction until one lower-bound outcome. |
| `Seek` | Compose guard/current-bound selection and checkpoint positioning. |
| Private `DecodeEntry` | Existing bounded physical header/payload primitive; no policy redesign. |
| `ReadEntry` | Decode one physical view with its prior-key prefix bound. |
| `ReconstructKey` | One prefix/suffix reconstruction shared by validation and iteration. |
| `ValidateRestartEntry` | One entry's pending restart matching transition. |
| `ValidateEntries` | Compose physical traversal, topology checks, reconstruction and visitation. |
| `CommitEntry` | Ordered iterator representation publication and checkpoint advancement. |
| `AdvanceRestartIndex` | Monotonic validated restart cursor advancement. |
| `ParseNextEntry` | Compose exhaustion, checked view/key-size validation and publication. |

DecodeVarint32, Block::Create, RestartKey, SeekToRestartPoint, reverse loops and
format/comparator adapters remain later recursive frontiers, not blanket
algorithm exemptions or whole-block certification.

## Characterization and delivery

Existing block tests cover byte-golden layouts, empty/zero restarts,
owned/borrowed moves, forward/reverse positioning, shrinking/growing binary keys,
accepted extended lengths, reached corruption/repositioning, every malformed
restart path, internal-key lengths, reverse comparator and random models.

Add a test-local counting comparator over a single-restart multi-entry block:
fresh seek visits the prefix, a forward seek from a later valid entry skips it,
and equal-current seek performs only its current comparison. Assert exact
comparison keys/order, not only returned lower-bound value.
Add physical mixed-fault priority and visitor short-circuit cases with exact
error text and visited entry sequence. Show restart rejection precedes visitor,
visitor failure suppresses later malformed entry/final restart checks, and
physical validation still does not validate key sort order.
Strengthen iterator key/value validity and recovery after a late restart error;
do not mutate backing storage under a live immutable block to manufacture reuse
evidence.

Characterizations run on original production before extraction. Rebuild the
actual unit target and select BlockTest, BlockIteratorTest, BlockBuilderTest,
BlockModelTest and coupled TableTest/CompactionTest as actual suite names permit.
Verify exact unique before/after JUnit sets and required names without failures,
errors or skips. Separate seek positioning and shared validation/publication
topic commits. Independent design review precedes accepted design merge;
independent combined review and exact-head successful CI precede normal delivery.
Formatting/docs/whitespace gates and existing coverage thresholds apply.

Independent bounded design review found no significant issue in checkpoint
reuse, checked representation sharing, error priority or ownership. It inspected
source/tests without edits or executing builds/tests. Accepted design #206
merged after all 20 exact-head checks succeeded.

## Implementation evidence

The four planned characterization cases passed on original production at
94cd0ed45ff78f0c03f7010ca6605523811a3fc8 before any extraction:
exact fresh/forward-reused/equal/backward comparison traces; decode-before-
restart-before-visitor fault priority; visitor failure suppressing later malformed
entry or final duplicate-restart checks; and a late out-of-range restart clearing
partially published key/value before explicit positioning restores earlier entries.
Existing unordered physical traversal and accepted extended encoding cases remain
in the same selection.

Checkpoint selection/restore/scan and shared checked-entry/validation/publication
were separate production topic commits after characterization. No public API,
builder, encoded bytes, comparator, backing ownership or coverage exclusions changed.
The iterator still validates internal-key length before entry-end calculation
and reserves before publishing current_; physical validation does not add reserve.
Original const Status copies remain in checkpoint decode/restore/scan and visitor
failure paths; new parent envelopes move or directly return children.

Rebuilt original/final block/builder/model/table/compaction selections contain
identical 129 unique JUnit cases, with no failures/errors/skips and all four required
new names present. The intermediate checkpoint/model selection passed 25 cases.
clang-format, documentation validation and whitespace gates apply before publication.
The 12-definition ledger above describes only this batch; the explicitly listed
remaining recursive frontiers are not certified by these results.

Independent combined review found no significant issue in the four changed
paths and directly confirmed the 129-case parity, four required characterizations,
25-case intermediate artifact and characterization-before-production commit order.
It did not edit files, execute builds/tests or independently establish execution
provenance, allocation-failure behavior or publication CI readiness.
