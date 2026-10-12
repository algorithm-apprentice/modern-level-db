# Bloom and Filter-Block Lifecycle Composition

## Scope and delivery

Design-first completion batch for table/bloom_filter.cc/.h and
table/filter_block.cc/.h.
Source baseline: `1512115cc56443631ce69e60638b32e3c8575954`.
Table opening/filter loading #180, table writer completion #182, block
compression #184, and complete block representation #260 remain unchanged
caller/evidence context.

This batch extracts shared Bloom probe representation and complete filter-block
layout admission, then gives every current Bloom/FilterBlock definition a
bounded semantic disposition. Keep reviewed design, original-source
characterization and two production topics as local commits, then publish one
non-draft PR.

No public API, Bloom bytes/probe count/false-positive policy, filter range,
offset-array format, builder overflow policy, block ownership/cacheability,
Table read/write routing, compression, allocation strategy, benchmark, runtime
assert, diagnostic conditional, coverage exclusion or threshold change.

## Bloom probe representation

CreateFilter and KeyMayMatch share four exact representation operations.
Add BloomHashDelta(hash), returning the existing 17-bit rotate-right delta.
Add BloomBitPosition(hash, bits), returning `hash % bits`; generated filters
have at least 64 data bits, while KeyMayMatch admits any two-byte filter and
therefore may query eight data bits. Both domains keep bits nonzero, so this
helper adds no zero guard.

Add SetBloomBit(filter, position), ORing BitMask(position) into the exact byte.
Add BloomBitIsSet(filter, position), testing that same byte/mask. These are
different mutation/query leaves over one wire representation, not a callback
framework.

CreateFilter retains size selection, output append/probe byte, one hash/delta
per key, probe count and `hash += delta` progression. It calls position/set
children. KeyMayMatch retains short-filter false, future-probe true, one
hash/delta, probe loop, first missing-bit false, and final true. It calls
position/query children. Do not normalize hashes, deduplicate positions,
precompute vectors or change false-positive behavior.

BloomFilterPolicy constructor retains the exact integer approximation
`bits_per_key * 69 / 100`, clamped to 1..30; do not replace it with a floating
ln(2) computation. FilterSize saturation/minimum, BloomHash, BitMask and Name
remain existing leaves/compositions.

## Filter-block layout admission and offset topology

Add file-local FilterBlockLayout with array_offset and filter_count, plus
FilterLayoutIssue with None, TooShort, UnsupportedRange and MalformedArray.
Add DecodeFilterLayout(data, layout), preserving exact priority:

1. Fewer than five bytes: TooShort.
2. Last byte not FilterRangeShift: UnsupportedRange.
3. Decode array offset; reject beyond offsets_end or non-fixed32 remainder:
   MalformedArray.
4. Publish complete layout and return None.

The child constructs no Error, owner or reader. FilterBlockReader::Create keeps
all exact Corruption messages/source locations in the parent, then moves
BlockContents into one reader only after layout success.

Add private OffsetsAreValid() const noexcept. It walks indexes zero through
filter_count (including the terminal array offset), requires first offset zero
and nondecreasing offsets, and returns false at the first violation.
Create constructs the reader, calls the predicate, creates the existing
`filter block offsets are invalid` Error in the parent, then returns the reader.

The vector overload remains the owner adapter. FilterOffset and KeyMayMatch keep
trusted layout decoding, beyond-last may-match policy and borrowed filter span.
Builder StartBlock/AddKey/Finish/GenerateFilter and overflow ordering remain
unchanged.

## Selected nine-definition ledger

| Definition | Composition or stopping boundary |
| --- | --- |
| BloomHashDelta | New rotate-derived probe delta leaf |
| BloomBitPosition | New hash-to-bit position leaf |
| SetBloomBit | New one-bit mutation leaf |
| BloomBitIsSet | New one-bit query leaf |
| BloomFilterPolicy::CreateFilter | Size/output/key/probe emission parent |
| BloomFilterPolicy::KeyMayMatch | Header/probe/missing-bit query parent |
| DecodeFilterLayout | New ordered trailer/array layout proof |
| FilterBlockReader::OffsetsAreValid | New zero-first/nondecreasing topology proof |
| FilterBlockReader::Create(BlockContents) | Layout/error/owner/topology parent |

## Twenty-five inspected unchanged definitions

### Bloom: five

| Definition | Responsibility retained |
| --- | --- |
| BloomHash | Hash key with persisted seed |
| BitMask | Map bit position to byte mask |
| BloomFilterPolicy constructor | Clamp exact integer bits-per-key * 69 / 100 to 1..30 |
| BloomFilterPolicy::FilterSize | Saturating count*bits, 64-bit minimum, byte/probe size |
| BloomFilterPolicy::Name | Persisted policy identity |

### FilterBlock: twenty

| Definition | Responsibility retained |
| --- | --- |
| BlockTooLarge | Construct exact builder overflow InvalidArgument |
| FilterBlockBuilder constructor | Copy policy |
| deleted builder copy constructor | Preserve unique buffers |
| deleted builder copy assignment | Preserve unique buffers |
| deleted builder move constructor | Keep borrowed Finish bytes stable |
| deleted builder move assignment | Keep borrowed Finish bytes stable |
| defaulted builder destructor | Release buffers |
| FilterBlockBuilder::StartBlock | Range index/finished-size proof and missing-filter generation |
| FilterBlockBuilder::AddKey | Finished-size proof then key offset/bytes append |
| FilterBlockBuilder::Finish | Final filter, offsets, array offset and range shift |
| FilterBlockBuilder::GenerateFilter | Flatten pending keys, call policy, clear pending state |
| FilterBlockReader::Create(vector) | Move vector into owned BlockContents adapter |
| deleted reader copy constructor | Preserve unique/borrowed contents representation |
| deleted reader copy assignment | Preserve unique/borrowed contents representation |
| defaulted reader move constructor | Transfer contents/policy/layout |
| deleted reader move assignment | Preserve stable representation semantics |
| defaulted reader destructor | Release optional contents owner |
| private reader constructor | Install contents/policy/layout scalars |
| FilterBlockReader::FilterOffset | Trusted fixed32 offset decode |
| FilterBlockReader::KeyMayMatch | Range routing, beyond-last true, selected-filter query |

The nine selected/new definitions plus these 25 rows yield 34 final named
definitions across the four files, with no lambdas.

## Independent design review

Independent source-backed review corrected two contract gaps before
implementation. The design now distinguishes generated minimum 64-bit filters
from accepted two-byte/eight-data-bit read filters and records the exact integer
probe formula. Characterization adds `bits_per_key 13 -> probes 8`, which
distinguishes it from floor(bits_per_key * ln2).

The layout matrix now also asserts that every Error is constructed by
FilterBlockReader::Create in filter_block.cc through both owned-vector and
borrowed-BlockContents entrypoints. No tests, builds or production edits were
performed during this bounded review.

## Original-source characterization

Existing Bloom tests cover persisted name/bytes, empty/small filters, probe
clamping, size/saturation, append prefix preservation, false-positive bounds and
future probe encodings. FilterBlock tests cover empty/golden ranges, overflow
admission, malformed trailer/layout/offset topology, LevelDB layouts and
borrowed reader moves.

Before production edits, add two cases:

1. For one literal key and policy 10, independently compute the six probe
   positions using Hash32 with the persisted seed and rotate progression.
   Require distinct expected positions, exact generated bits, and for each
   position clear only that bit in a copy and require KeyMayMatch false.
2. A filter layout mixed-fault matrix with exact messages and priority:
   too-short beats range/array, unsupported range beats malformed array,
   malformed array beats offset topology, then valid layout with first-nonzero
   or decreasing offsets reports invalid offsets. Exercise both owned and
   borrowed Create paths for every stage and assert file `filter_block.cc` and
   function `FilterBlockReader::Create` source provenance.

Also strengthen the existing probe-count table with `13 -> 8`.

Use public operations and literal/test-side hash math, not private helper hooks.
Do not claim false-positive elimination, allocation failure or backing lifetime
beyond the existing owner contracts.

Freshly build modern_leveldb_unit_tests and the Windows native child on
unchanged production. Freeze exact unique identities for BloomFilterTest,
FilterBlockTest, FilterBlockReaderTest, BlockCompressionTest, TableBuilderTest,
TableTest, TableNativeTest, TableCacheTest, LookupTest and DatabaseTest.
Require every suite to contribute a nonzero identity count.

Independently review new cases before production edits. Commit Bloom and filter
layout topics separately, checking exact relevant original subsets. Finish with
full identity parity and complete GCOVR/LCOV/NOLINT annotation multiset equality
for all four production paths.

Local GCC Release warnings-as-errors should compile all changed/coupled objects
with `-fno-range-for-ext-temps`. Independent integrated review must reconcile
all 34 definitions, exact hash/probe progression, layout issue/source-location
behavior, owned/borrowed contents and prior Table/compression boundaries.
One published exact head must pass every hosted check, authoritative GCC 13
branch coverage and actual Windows Debug original-name parity before normal
squash delivery.

Successful tests do not prove arbitrary allocation failure, false-positive
rates outside the existing statistical case, borrowed backing mutation,
unsupported builder reuse, performance, or whole-production completion.
