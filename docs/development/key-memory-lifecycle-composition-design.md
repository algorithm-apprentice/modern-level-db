# Internal-Key and Arena-Backed MemTable Lifecycle

## Scope and delivery

Design-first completion batch for format/internal_key.cc/.h,
memory/arena.cc/.h and memory/memtable.cc/.h.
Source baseline: `526473de51c5ac8444ddafd5aea2bb4807b18e3e`.
Prior deliveries already cover key comparison/shortening #245, lookup storage/
trusted encoding #250, read-record/MemTable lookup #254, and complete SkipList
publication/search #256.

This batch closes the remaining checked internal-key decode issue pipeline,
composes Arena alignment/admission/refill policy, and gives every unchanged
MemTable body a bounded semantic disposition. Keep reviewed design,
original-source characterization and two production topics as local commits,
then publish one non-draft PR.
The design and original characterization are accepted; both production topics
are implemented locally. Integrated review is complete; delivery remains
pending.

Changed production paths are internal_key.cc and arena.cc/.h. internal_key.h and
memtable.cc/.h are inspected but unchanged. No public API, key bytes/order,
LookupKey storage, MemTable packing/search, SkipList behavior, Arena block size/
threshold/accounting, allocation owner, sequence/kind policy, error text,
benchmark, runtime assert, diagnostic conditional, coverage exclusion or
threshold change.

## Checked internal-key decode issue

The current private TryDecodeInternalKey performs trailer presence, fixed64
decode, kind decode/admission, and successful aggregate publication.
ParseInternalKey calls it, then rechecks encoded size to distinguish the two
public Corruption messages. Comparator shortening uses only the boolean form.

Add file-local InternalKeyDecodeIssue with None, ShortTrailer and UnknownKind.
Add DecodeInternalKeyView(encoded, decoded), preserving exact priority:

1. Shorter than InternalKeyTrailerSize returns ShortTrailer without decoding.
2. Decode the complete trailer and kind.
3. Unsupported kind returns UnknownKind without publishing decoded.
4. Publish user_key, sequence and kind together, then return None.

TryDecodeInternalKey becomes the compatibility boolean adapter used by
shortening. ParseInternalKey calls the same child once, creates both Errors in
the parent so their source_location remains ParseInternalKey, and returns the
borrowed view on None. Do not add an Error to the hot boolean path, allocate,
copy backing bytes, inspect user bytes, or change aggregate publication.

InternalKey::Decode retains ParseInternalKey admission followed by one owned byte
copy. Its original failed Error copy remains unchanged. EncodeUnchecked,
trusted emission, LookupKey lifecycle, construction admission, accessors and
comparator/shortening remain exactly their delivered operations.

## Arena alignment, admission and refill

Add AlignmentPadding(pointer), returning zero for null/aligned pointers and
otherwise Alignment minus low-bit misalignment. Preserve uintptr_t conversion
and the power-of-two Alignment invariant; no runtime guard.

Add CanAllocateFromCurrentBlock(bytes, padding), using the established Arena
cursor invariant: a live current pointer and bytes_remaining_ advance/decrease
by the same amount from an aligned 4 KiB block, so alignment padding equals
`bytes_remaining_ % Alignment` and never exceeds bytes_remaining_; the empty
state is pointer null, remaining zero, padding zero. The child therefore uses
the single reachable admission `bytes <= bytes_remaining_ - padding`.
AllocateAligned composes padding selection, current-block admission, and
existing fallback. It returns the exact aligned current pointer when admitted.
Do not retain the old redundant `padding <= remaining` runtime branch in newly
extracted hot-path code or manufacture invalid private state to cover it.

Add ShouldAllocateDedicated(bytes), retaining the strict
`bytes > DedicatedAllocationThreshold` policy. Exactly 1,024 bytes refills/
uses the standard 4 KiB block; 1,025 receives a dedicated block only when the
current block cannot satisfy it.

Add RefillCurrentBlock(), assigning AllocateBlock(BlockSize) then
bytes_remaining_=BlockSize in the original order. AllocateFallback composes
dedicated selection or refill followed by AllocateFromCurrentBlock.
Large dedicated allocations do not replace or discard the current small block.

Allocate/AllocateFromCurrentBlock/AllocateBlock and memory_usage retain their
existing ownership/accounting contracts. A failed block allocation publishes
no pointer/vector/accounting mutation beyond standard expression sequencing;
no catch, fallback owner or zero-size contract is added.

## MemTable complete bounded disposition

MemTable stores private packed records in Arena and indexes record pointers in
SkipList. Add retains encoded-size calculation, Arena allocation/record
emission, then unique-key insertion. Lookup retains iterator lower bound,
decoded record, comparator equality and Value/Deletion classification from
#254. Iterator adapters decode trusted packed entries or construct one private
length-prefixed seek key.

ReadPin is a nonowning externally synchronized count: construction increments,
move transfers/nulls source, destruction decrements only a live table pointer.
WriteBufferState owns any rotated table needed by pins. No shared owner, atomic
counter, move assignment, guard or lifetime extension is introduced.

All inline accessors/deleted special members remain lifetime/type contracts.
This batch changes no MemTable production code; its complete ledger is static
source review integrated with Arena/internal-key runtime evidence.

## Selected nine-definition ledger

| Definition | Composition or stopping boundary |
| --- | --- |
| DecodeInternalKeyView | New ordered trailer/kind proof and atomic borrowed-view publication |
| TryDecodeInternalKey | Boolean compatibility adapter over decode issue |
| ParseInternalKey | One decode issue mapping to exact parent-source Corruption |
| Arena::AlignmentPadding | New null/aligned/misaligned padding leaf |
| Arena::CanAllocateFromCurrentBlock | New invariant-backed padded-size admission |
| Arena::ShouldAllocateDedicated | New strict threshold policy leaf |
| Arena::RefillCurrentBlock | New standard-block owner/cursor refill |
| Arena::AllocateAligned | Padding/admission/fallback parent |
| Arena::AllocateFallback | Dedicated or refill/current-block parent |

## Fifty-four inspected unchanged internal-key definitions

| Definition | Responsibility retained |
| --- | --- |
| IsValidValueKind | Admit Deletion or Value |
| DecodeValueKind | Byte-to-kind representation projection |
| ConsumeValueKindTrusted | Decode present kind and advance one byte |
| PackInternalKeyTrailer | Pack 56-bit sequence and kind |
| DecodeInternalKeySequence | Extract high 56 sequence bits |
| DecodeInternalKeyValueKind(uint64) | Extract/decode low kind byte |
| DecodeInternalKeyTrailer | Decode complete fixed64 trailer |
| DecodeInternalKeyValueKind(ByteView) | Trailer decode then kind projection |
| ValidateInternalKeyInput | Sequence-before-kind construction admission |
| EncodeUnchecked | Reserve/copy user bytes/append packed trailer owner |
| CompareDescendingTrailers | Decode both trailers and reverse unsigned order |
| IsShorterAndGreaterUserKey | Size-first/user-comparator eligibility |
| AppendMaxSeekTrailer | Append packed maximum seek trailer |
| CommitSeparatorBeforeLimit | Complete-key limit admission before move publication |
| EncodeInternalKeyTrusted | Borrowed user/trailer emission and cursor advance |
| LookupKey::Create | Checked lookup sequence admission |
| LookupKey::CreateTrusted | Layout/storage/prefix/shared body parent |
| LookupKey::PrepareStorage | Fresh inline/heap representation selection |
| deleted LookupKey copy constructor | Preserve unique backing owner |
| deleted LookupKey copy assignment | Preserve unique backing owner |
| defaulted LookupKey destructor | Release optional heap owner |
| LookupKey default constructor | Install canonical empty representation |
| LookupKey move constructor | Transfer representation then reset source |
| LookupKey move assignment | Self guard, transfer, canonical source reset |
| LookupKey::TransferStorageFrom | Inline/heap/size/offset transfer |
| LookupKey::memtable_key | Borrow complete prefixed bytes |
| LookupKey::internal_key | Borrow suffix after stored prefix offset |
| LookupKey::user_key | Borrow internal key before trailer |
| LookupKey::data mutable | Select heap or inline writable storage |
| LookupKey::data const | Select heap or inline readable storage |
| LookupKey::ResetToCanonicalEmpty | Release heap then canonicalize inline state |
| LookupKey::InitializeCanonicalEmpty | Install exact nine-byte empty seek key |
| InternalKey::Create | Ordered input proof then owned unchecked encoding |
| InternalKey::Decode | Checked parse then owned complete-byte copy |
| defaulted InternalKey copy constructor | Copy owning encoded vector |
| defaulted InternalKey copy assignment | Copy owning encoded vector |
| defaulted InternalKey move constructor | Move owning encoded vector |
| defaulted InternalKey move assignment | Move owning encoded vector |
| defaulted InternalKey destructor | Release encoded vector |
| InternalKey::encoded | Borrow complete owned encoding |
| private InternalKey vector constructor | Move one complete encoded owner |
| InternalKey::user_key | Borrow bytes before trailer |
| InternalKey::sequence | Decode sequence from owned trailer |
| InternalKey::kind | Decode kind from owned trailer |
| InternalKey::trailer | Decode owned fixed64 trailer |
| InternalKeyComparator constructor | Borrow one stable user comparator |
| deleted comparator-rvalue constructor | Reject temporary comparator |
| deleted const-comparator-rvalue constructor | Reject const temporary comparator |
| InternalKeyComparator::Compare(ByteView) | User order then descending trailer |
| InternalKeyComparator::Compare(InternalKey) | Owned-key adapter |
| InternalKeyComparator::Name | Persisted comparator identity |
| InternalKeyComparator::FindShortestSeparator | Checked pair/candidate/eligibility/trailer/limit commit |
| InternalKeyComparator::FindShortSuccessor | Checked key/candidate/eligibility/trailer/move |
| InternalKeyComparator::user_comparator | Borrow underlying comparator |

## Ten inspected unchanged Arena definitions

| Definition | Responsibility retained |
| --- | --- |
| defaulted Arena constructor | Initialize empty monotonic owner |
| deleted Arena copy constructor | Preserve unique block ownership |
| deleted Arena copy assignment | Preserve unique block ownership |
| deleted Arena move constructor | Keep returned storage addresses stable |
| deleted Arena move assignment | Keep returned storage addresses stable |
| defaulted Arena destructor | Reclaim all owned blocks together |
| Arena::memory_usage | Read reserved-capacity/pointer accounting |
| Arena::Allocate | Current-block fast path or fallback |
| Arena::AllocateFromCurrentBlock | Coupled pointer/remaining bump allocation |
| Arena::AllocateBlock | Allocate owner, publish vector, add exact accounting |

## Forty-two inspected unchanged MemTable definitions

| Definition | Responsibility retained |
| --- | --- |
| EntryComparator constructor | Own internal comparator borrowing user comparator |
| EntryComparator::operator() | Decode two packed internal keys and compare |
| EntryComparator::user_comparator | Borrow configured user comparator |
| deleted comparator-rvalue MemTable constructor | Reject temporary comparator |
| deleted const-comparator-rvalue constructor | Reject const temporary comparator |
| deleted MemTable copy constructor | Preserve Arena/SkipList identity |
| deleted MemTable copy assignment | Preserve Arena/SkipList identity |
| deleted MemTable move constructor | Preserve record/link addresses |
| deleted MemTable move assignment | Preserve record/link addresses |
| defaulted MemTable destructor | Destroy SkipList then Arena owners |
| MemTable constructor | Install comparator and Arena-backed table |
| DecodeMemTableInternalKey | Trusted first length-prefixed field borrow |
| DecodeMemTableEntry | Trusted internal-key/value pair borrow |
| EncodedMemTableEntrySize | Complete packed record size expression |
| EncodeMemTableEntry | Arena allocation and ordered prefix/key/value emission |
| MemTable::Add | Encode one trusted entry then publish unique pointer |
| MatchesMemTableUserKey | Strip trailer and compare one user key |
| ClassifyMemTableEntry | Value/Deletion projection with borrowed value |
| MemTable::Lookup | Seek, equality and classification parent |
| MemTable::memory_usage | Arena accounting adapter |
| deleted ReadPin copy constructor | Prevent duplicated counter release |
| deleted ReadPin copy assignment | Prevent duplicated counter release |
| deleted ReadPin move assignment | Preserve one transfer-only pin |
| ReadPin constructor | Borrow table and increment count |
| ReadPin move constructor | Transfer pointer and null source |
| ReadPin destructor | Decrement only a live borrowed table |
| ReadPin::value | Borrow pinned table |
| MemTable::PinRead | Construct one externally synchronized pin |
| Iterator constructor | Borrow table SkipList |
| deleted Iterator copy constructor | Preserve one cursor state |
| deleted Iterator copy assignment | Preserve one cursor state |
| deleted Iterator move constructor | Preserve one cursor state |
| deleted Iterator move assignment | Preserve one cursor state |
| defaulted Iterator destructor | Release seek buffer/cursor only |
| Iterator::valid | Delegate SkipList validity |
| Iterator::key | Decode/borrow packed internal key |
| Iterator::value | Decode/borrow packed value |
| Iterator::Next | Delegate level-zero successor |
| Iterator::Prev | Delegate strict predecessor |
| Iterator::Seek | Build private length-prefixed key then seek |
| Iterator::SeekToFirst | Delegate first positioning |
| Iterator::SeekToLast | Delegate last positioning |

The nine selected/new definitions plus 106 unchanged rows yield 115 final named
definitions across the six files, with no current lambdas. Prior delivered
designs remain evidence for their rows; this batch reconciles rather than
reimplements them. Counts are bounded ledger bookkeeping, not whole-production
completion.

## Independent design review

Independent source-backed review found that mechanically extracting the
baseline compound Arena admission would create an unreachable changed branch:
under every supported state padding cannot exceed bytes_remaining_. The design
now states and relies on that internal invariant and keeps only the reachable
size-after-padding comparison. No private hook, coverage exclusion, assertion or
invalid-state behavior is added. No tests, builds or production edits were
performed during this bounded review.

## Original-source characterization

Existing suites already cover key golden encodings, sequence/kind boundaries,
truncated/unknown kinds, borrowed parsed views, LookupKey moves/canonical state,
comparator shortening, Arena block growth/alignment/dedicated thresholds,
SkipList publication, MemTable packed records, pins, snapshots, iterators,
retained values and database reads.

Before production edits, add two cases:

1. Parse every encoded size 0-7 and one complete unknown-kind trailer. Assert
   exact Corruption message and source location function `ParseInternalKey` for
   ParseInternalKey and the copied InternalKey::Decode failure. Then parse one
   literal binary user key/max sequence/Deletion and prove complete borrowed
   publication only on success.
2. After one unaligned Arena byte, AllocateAligned exactly `remaining-padding`
   bytes and prove it fits the original block/aligned address. In a fresh Arena,
   request one byte more and prove fallback uses a separate dedicated block,
   preserves the first byte/current small block, and reports exact accounting.

Use public/internal subsystem APIs, not private helper hooks. Literal internal
key bytes must not be produced by production encoders. Do not infer arbitrary
allocator failure or unsupported zero-size behavior.

Freshly build modern_leveldb_unit_tests and the Windows native child on
unchanged production. Freeze exact unique identities for InternalKeyTest,
InternalKeyComparatorTest, LookupKeyTest, ArenaTest, SkipListTest, MemTableTest,
WriteBatchTest, TableTest, TableNativeTest, TableCacheTest, LookupTest,
RecoveryTest and DatabaseTest.

Independently review new cases before production edits. Commit checked decode
and Arena policy topics separately, checking exact relevant original subsets.
Finish with full identity parity and complete GCOVR/LCOV/NOLINT annotation
multiset equality for all six production paths.

Local GCC Release warnings-as-errors should compile all changed/coupled objects
with `-fno-range-for-ext-temps`. Independent integrated review must reconcile
all 115 rows, decode issue/source location/error-copy behavior, Arena pointer/
padding/accounting order, MemTable borrowed owners and every prior delivered
boundary. One published exact head must pass every hosted check, authoritative
GCC 13 branch coverage and actual Windows Debug original-name parity before
normal squash delivery.

Successful tests do not prove arbitrary allocation failure, physical copy/move/
destruction counts, unsupported zero-size allocation, concurrent Arena writers,
pins outliving MemTable owners, performance, or whole-production completion.

## Local implementation evidence

The accepted design is commit `1be03ae`. Original-source characterization is
split before production edits: `c1e6ace` covers decode issue/message/location
and complete borrowed publication; `9c5bdda` covers exact padded fit and
one-byte-over dedicated fallback. Independent review found no significant issue.

A fresh unchanged-production Windows Debug build froze 200 unique identities:

| Suite | Cases |
| --- | ---: |
| InternalKeyTest | 10 |
| InternalKeyComparatorTest | 10 |
| LookupKeyTest | 6 |
| ArenaTest | 9 |
| SkipListTest | 11 |
| MemTableTest | 13 |
| WriteBatchTest | 9 |
| TableTest | 38 |
| TableNativeTest | 2 |
| TableCacheTest | 7 |
| LookupTest | 14 |
| RecoveryTest | 17 |
| DatabaseTest | 54 |

The production topics remain separate local commits. `1129e49` composes one
internal decode issue proof, boolean compatibility and Parse parent mapping; its
four-suite selection is the exact original 39-identity subset. `e729b5d`
composes Arena padding, invariant-backed admission, strict threshold and refill;
its Arena/SkipList/MemTable selection is the exact original 33-identity subset.
The final integrated selection exactly matches all original 200 identities,
with no failure, error, skip or duplicate.

Complete GCOVR/LCOV/NOLINT annotation multisets are unchanged across all six
production paths. The expanded final scan reports 115 named definitions, zero
lambdas and zero recovery nodes, matching the complete ledger. Local GCC
Release warnings-as-errors with `-fno-range-for-ext-temps` compiles all three
production objects and every portable changed/coupled test translation unit.
Documentation validation and `git diff --check` pass. Hosted GCC 13 branch
coverage, Windows native execution/name parity and normal merge remain delivery
obligations.

Independent integrated review found no significant issue across the complete
diff and 115-definition ledger. The final Windows Debug 200-test selection
passed during review. Historical intermediate artifacts and hosted GCC 13
coverage were not independently reproduced; the known unrelated local GCC 16
compaction warning, arbitrary allocation failure, zero-size behavior,
concurrent Arena access, pins outliving owners, performance and whole-production
behavior remain outside this bounded conclusion.
