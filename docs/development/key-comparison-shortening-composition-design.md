# Key Comparison, Shortening and Prefix Composition

## Status and scope

Proposed recursive follow-up to the production composition plan.
Source-read baseline: `9a01cafd87a983fecaad7cc4aff6561e3cc2525c`, after
block reverse #241. Pending hash #243 uses independent source/test/design
paths and is not a code prerequisite.

Three responsibility-sized topics share one design/implementation PR rather
than separate CI runs: normalized byte/trailer comparisons; checked bytewise
and internal-key shortening; prefix-compressed builder entry/restart stages.
Touch only base/comparator.cc, format/internal_key.cc, table/block_builder.cc,
existing base/bytes_internal.h and block_builder.h, their three existing test
files, and this document. No public API, key ordering, wire format, allocator,
recovery, coverage policy or benchmark changes.

Prior-art search finds two identical bounded prefix loops: bytewise separator
and BlockBuilder::Add. Existing base/bytes_internal.h already houses a narrow
borrowed-byte operation shared across subsystems. Add one inline
SharedPrefixLength(ByteView, ByteView) there, not a public utility or a second
private implementation. Preserve minimum bound, forward equality reads and
return the matched count. BlockBuilder invokes it only outside a restart.
Existing CopyLookupValue stays unchanged with its separate live-owner contract.

## Normalized comparisons

Bytewise Compare retains const equal-prefix order, early nonzero return, then
length tie-break. CompareCommonPrefix owns the original const minimum size and
const memcmp result, including the zero-size gate that avoids null-storage
memcmp. NormalizeByteOrder maps negative/positive/zero to exactly -1/1/0.
CompareKeyLengths preserves the original length ordering. These bounded
comparison, scalar-sign representation and scalar-length-order jobs have no
new owners, allocation or failure guard; retain noexcept.
Existing exhaustive byte-pair, scalar-reference, null/unaligned/overlapping
and prefix tests remain the main normalized-order oracles.

InternalKeyComparator::Compare retains its two user-key subviews, const
virtual user order and early nonzero return. Only then call
CompareDescendingTrailers, retaining the original two const uint64 decodes
through DecodeFixed64(last<InternalKeyTrailerSize>()) and >/< return order.
Do not add malformed-input checks to this established comparator domain or
replace a user comparator's nonzero magnitude with normalized bytewise order.
Its arbitrary user order must pass through unchanged.

## Bytewise shortening and shared prefix mutation

FindShortestSeparator keeps the original minimum-size local and prefix gate.
SharedPrefixLength computes its own bounded count; the retained gate is
difference >= minimum_size. Extra trivial span/size scalar computations are
not observable callbacks and are not certified as identical physical reads.
Retain const start/limit unsigned bytes before any mutation.
SeparatorSuccessor selects an optional encoded next byte using exactly
start_byte < ff and start_byte + 1 < limit_byte. No capacity growth or new
fallback policy. The optional is a non-owning, nonallocating scalar selector;
it is not an Error/Status boundary.

FindByteSuccessor scans the same ordered bytes, computes each original unsigned
value once, and returns an optional ByteSuccessor containing index and encoded
next byte at the first value != ff. Declare the aggregate in its own statement.
All-ff and empty inputs return no position without mutation.
FindShortSuccessor selects that point and invokes CommitShortKey.
Both parents share CommitShortKey(key, index, successor_byte): store the
already-computed byte and resize to index + 1, in that order.
Proven selected indices are in range and only shrink the default byte vector.
Keep the original public throwing signatures; do not introduce catches,
rollback, repeated reads of a borrowed alias, or noexcept for vector mutation.
A limit may validly borrow start storage during selection; never read it after
the prefix commit invalidates the corresponding logical elements.

## Internal-key eligibility and publication

Both shortening parents retain checked TryDecodeInternalKey gates, borrowed
decoded views and locally owned candidate vectors through function exit.
Separator keeps its original short-circuit start-then-limit decode order.
Neither moves an owner or publishes start/key while invoking the user
shortener or proving eligibility.

IsShorterAndGreaterUserKey borrows comparator/original/candidate, preserving
the size-first rejection and only then Compare(original, candidate) >= 0.
It does not normalize the user's order, compare equal/longer candidates, or
append a trailer before that call. After eligibility, AppendMaxSeekTrailer
shares the identical PackInternalKeyTrailer(MaxSequenceNumber, SeekValueKind)
and AppendFixed64 policy used by both parents.

Separator then invokes CommitSeparatorBeforeLimit with the borrowed internal
comparator, original vector, candidate and limit: the original internal
Compare(candidate, limit) must be strictly negative before vector move.
That internal comparison may perform another virtual user comparison and
equal-user descending-trailer comparison. Do not reduce it to a user-key-only
test. Successor needs no upper limit and retains its direct final vector move.
User-shortener exceptions propagate with the original input vector unchanged;
all existing parent candidate/view lifetimes and allocation timing remain.
Non-owning helper arguments/scalar copies are not physical-copy certification.
No private/public comparator header additions are necessary.

## Builder entry and restart composition

Keep Add's original parent locals and order: const restart decision; shared
count initialized to zero and computed only for non-restart; const non_shared;
restart push/reset; three header writes; const key_delta subview; key payload
then value payload; prior-key update; entry-count increment.
SharedPrefixLength observes the previous key only before these mutations.
Do not acquire key_delta early, move it into another owner, or introduce
decoder/callback/rollback framework.

BeginRestart pushes the current buffer offset before resetting the entry
count. AppendEntryHeader retains shared/non_shared/value uint32 casts and the
same three AppendVarint32 calls. AppendEntryPayload retains ordered key/value
inserts. CommitEntryHistory retains last_key resize, delta insert, then count
increment, so a throwing insert cannot advance the counter.
These methods borrow parent scalars/views and mutate the existing builder.
Add/Finish throwing requires Reset exactly as the existing header documents;
do not create stronger exception safety or invalid-interval behavior.

Finish calls AppendRestarts (leading zero then existing ordered restarts), then
the original restart-count AppendFixed32, and returns the same borrowed buffer.
Do not change zero/empty representation, estimates, reset or ownership.
Keep all selected methods' original exception category; no new runtime guards
or assertions. Restart interval must be >= 1, sorted keys/uint32 bounds apply,
and Add/Finish after Finish requires Reset. New tests must respect those
existing caller contracts rather than characterize unsupported calls.

## Recursive definition ledger

| Definition | Children or responsibility-based stopping reason |
|---|---|
| `BytewiseComparatorImpl::Compare` | Prefix relation, then length tie-break. |
| `CompareCommonPrefix` | Bounded memcmp selection and normalized sign. |
| `NormalizeByteOrder` | One negative/zero/positive scalar-sign projection. |
| `CompareKeyLengths` | One normalized unsigned length ordering. |
| `SharedPrefixLength` | Bounded forward matched-byte count shared by two real callers. |
| `BytewiseComparatorImpl::FindShortestSeparator` | Prefix position, strict separator-byte admission, shared prefix commit. |
| `SeparatorSuccessor` | One strict byte interval admission yielding the encoded successor. |
| `BytewiseComparatorImpl::FindShortSuccessor` | First incrementable position and shared prefix commit. |
| `FindByteSuccessor` | Bounded ordered byte-successor selection or absence. |
| `CommitShortKey` | Coupled successor-byte store and suffix truncation. |
| `InternalKeyComparator::Compare` | User ordering before descending trailer tie-break. |
| `CompareDescendingTrailers` | Existing trailer decodes and one reversed unsigned relation. |
| `InternalKeyComparator::FindShortestSeparator` | Checked pair, owned candidate, user shortening, eligibility, trailer, bounded commit. |
| `InternalKeyComparator::FindShortSuccessor` | Checked key, owned candidate, user shortening, eligibility, trailer, final move. |
| `IsShorterAndGreaterUserKey` | One size-first/user-order candidate eligibility decision. |
| `AppendMaxSeekTrailer` | Pack/append the shared maximum-seek trailer policy. |
| `CommitSeparatorBeforeLimit` | Strict complete internal-key upper-bound admission before publication. |
| `BlockBuilder::Add` | Prefix decision, optional restart, header, payload, entry history. |
| `BeginRestart` | Coupled offset insertion/count reset in original throwing order. |
| `AppendEntryHeader` | Ordered three-field compressed-entry header using existing varint primitives. |
| `AppendEntryPayload` | Ordered key-delta/value payload insertion. |
| `CommitEntryHistory` | Coupled previous-key reconstruction/count commit. |
| `BlockBuilder::Finish` | Restart-array emission, count trailer, borrowed buffer return. |
| `AppendRestarts` | One implicit-zero plus stored-offset wire array. |

Unchanged Bytewise Name returns the persisted identity; BytewiseComparator
returns its existing static borrowed singleton. Builder constructor installs
the interval, Reset clears the coupled block/history state, CurrentSizeEstimate
computes one wire-size expression, and inline empty reads the history flag.
Those stopping reasons apply to these specific bodies, not all short functions.
Other internal_key definitions, coding primitives and production/header/lambda
frontiers remain in the every-definition audit.

## Original-source characterization and review

Before production edits, retain all existing normalized/scalar/golden cases and
add six public cases, independently reviewed on original source:

1. Separator boundary matrix with empty/prefix/equal/adjacent/high bytes and
   a valid limit borrowing start storage; literal unchanged/shortened bytes.
2. Empty/all-ff/first-later incrementable successor byte matrix, including
   fe -> ff and embedded zero; fixed output vectors.
3. Internal separator/successor candidate size/order/upper-bound matrix with
   exact nonallocating callback recording and literal complete output/trailer.
   Include an equal complete internal limit and arbitrary nonzero user-order
   magnitude passthrough; distinguish eligibility from internal final compare.
4. Malformed input and throwing-shortener boundaries: no callback on rejected
   checked keys, exact callback order and no early original-vector publication.
   Compare is noexcept, so never throw or allocate a recorder there.
5. Sorted empty/zero/high binary keys and values across valid restart intervals
   1/2/3, with independently assembled literal full blocks and exact estimates.
6. Reset after an owned copy of Finish restores empty prefix/restart history
   and exact subsequent literal bytes. Never read the invalidated old view.

Build modern_leveldb_unit_tests and modern_leveldb_windows_lock_child.
Coupled anchored native selector:
`^(ComparatorTest|InternalKeyTest|InternalKeyComparatorTest|LookupKeyTest|BlockBuilderTest|BlockTest|BlockIteratorTest|BlockModelTest|TableTest|TableNativeTest|TableBuilderTest|DbIteratorTest|DbIteratorSamplingTest|MergingIteratorTest|LevelIteratorTest|CompactionTest|DatabaseTest)\.`
Measure actual unique counts/nonempty classes, additions and zero failures/
errors/skips; final identities must equal original.
After comparison and shortening topics select
`^(ComparatorTest|InternalKeyTest|InternalKeyComparatorTest|LookupKeyTest)\.`
Do not reuse hash96 or reverse201 as this new baseline.

Independent design acceptance precedes cases; independent characterization
review precedes the three source topics; integrated read-only code/evidence
review precedes non-draft publication. Require all exact reviewed-head checks,
actual hosted Windows Debug selected-name parity, normal merge, reviewed-path
byte equality and branch cleanup. Diff-cover/benchmark thresholds remain
unchanged. Source review supports private ordering/lifetimes, not hypothetical
physical copy/elision, allocator observation or performance equivalence.
This selected batch cannot certify the remaining production-wide audit.

### Independent design review

The bounded read-only design reviewer traced the scoped source, actual headers,
existing characterization and supported caller contracts. No significant issue
was found; the six planned groups materially extend existing coverage.
The reviewer reported existing binary discovery of 230 coupled identities and
31 foundation identities, but explicitly did not establish binary-to-baseline
identity or passing results. These are not the expected original-source totals:
the current binaries were last built on the independent hash branch, without
#241's two block cases. Rebuild the accepted source before measuring the new
baseline. Implementation, hosted checks, coverage and performance remain
future gates; no edits/builds/native execution or nested agents were performed
by this reviewer.
