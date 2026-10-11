# Table Opening and Lookup Composition

## Scope and delivery

Design proposal for T01/T02/T03 from the
[production composition plan](production-function-composition-plan.md).
The source baseline is `5353f92c400291474ebce5f1d2711b8144c62ba8`.
This batch changes private organization in `src/table/table.cc` and its directly
dependent private declarations in `table.h`, characterization and this document.
No production implementation is included in this design PR.

Independently review and merge this design before implementing. Use small
topic commits for characterization, opening, filters, and lookup, but one
combined implementation PR with independent whole-diff review and exact-head CI.
Do not change codecs, checksums, block parsing, cache admission, table iteration,
native mapping, public APIs, engine policies, benchmarks, or coverage policy.

## Existing contracts and reusable primitives

`Table::Open` rejects null files and sizes below `FooterSize` before reading.
It reads/decodes the footer, loads/physically validates the index and its data
handles, optionally loads the metaindex/filter, assigns cache identity, and
constructs the owning table, in that order. The unique file retains borrowed
mapped index/meta/filter/data bytes. Comparator and block cache remain borrowed.
No cache identity or table is published on an earlier failure.

Reuse `ReadExactly`, `ReadStoredBlock`, `ReadBlock`, `ValidateIndex`,
`ConsumeBlockHandle`, `DecodeFooter`, `FilterBlockReader::Create`, and existing
block iterators. Do not create another read/decode stack. Footer reads remain
copied reads even when block views are available. Short-read completion,
oversized reads, truncated files, block-region checks, checksum/codec dispatch
and cacheability stay in their existing primitives.

Index validation depends on Internal/Arbitrary key mode and checks monotonic
nonoverlapping data handles. Metaindex validation checks its physical entry and
restart topology, not index data-handle semantics or whole-file correctness.
With no configured filter policy, Open does not read/validate metaindex or filter
contents. With a policy, an absent or differently named filter is successful
absence, not corruption.

`Table::Get` seeks the index and consults the filter before acquiring a data
block. It seeks only that selected block, parses the candidate internal key,
compares user keys with the configured user comparator, handles deletion, and
copies a value to caller-owned storage before releasing its block pin.
Missing, deletion, and errors leave the supplied value vector unchanged.
Read options/cache identity and mapped backing lifetimes are unchanged.
Get does not advance to the next data block after a candidate miss.

## T01: footer and validated index acquisition

Keep null/size guards and `blocks_end` computation in Open.
Extract file-local `ReadFooter(file, blocks_end)` to compose fixed-size
`ReadExactly` and `DecodeFooter`, preserving the existing failure order.
Extract `ReadValidatedIndex(file, blocks_end, handle, key_format)` to compose
`ReadBlock` and `ValidateIndex` with the original key mode.

Open composes those children, optional filtering, existing cache-ID selection,
and unchanged owner construction. Borrow the still-owned file in helpers; do not
transfer it until table construction. Move errors through new Result-returning
layers so extraction introduces no additional Error copies or catches.
Keep the existing constructor/allocation coverage rationale; do not add blanket
function exclusions or new defensive guards below established boundaries.

## T02: optional filter selection and loading

Keep ReadFilter's output optional and success-on-absence contract.
Extract `ReadValidatedMetaindex` to compose `ReadBlock` with the original
physical `ValidateEntries` callback, not `ValidateIndex`.
Name the policy key grammar with `FilterMetaindexKey`: fixed `filter.` prefix
plus the existing policy name, using ordinary string primitives.

`FindFilterHandle(meta, policy)` composes key construction, checked/validated
metaindex seek, exact name comparison, and `DecodeFilterHandle`.
Its result is an optional handle: missing position/name means absence; selected
malformed handle is the original Corruption, not absence.
`DecodeFilterHandle` consumes one existing encoded handle and requires no trailing
bytes, keeping `table filter handle is malformed`.
Retain the existing infallible-seek exclusion only at the physically validated
seek boundary; its rationale does not exempt the rest of filter processing.

Extract `ReadFilterBlock` to compose `ReadStoredBlock` and
`FilterBlockReader::Create`. ReadFilter composes metaindex acquisition, optional
selection, complete reader construction and final optional emplacement.
No filter storage is read for absent/mismatched entries. No errors become default
readers or silently absent filters. Borrowed metaindex bytes stay owned through
seek and handle decoding; the returned handle owns its scalar fields.
The filter's stored backing remains owned or retained by the table file.

## T03: pinned data-block lookup

Get keeps index/filter routing and `ReadDataBlock` acquisition/error handling.
Extract private const `LookupDataBlock(const Block&, const LookupKey&,
std::vector<std::byte>&)` to compose data seek, checked candidate parsing,
user-comparator equality, deletion dispatch, and owned-value copy.
Get owns the BlockReference until this helper returns; the helper returns no
borrowed data. Existing cache/pin behavior and block key mode remain explicit.

Name `CopyLookupValue` as the coupled owned-copy operation implemented by the
original vector resize and byte copy. Do not substitute vector assignment that
could change capacity/allocation behavior. Neither helper clears output on a
non-value result. Exceptions still propagate without conversion/catches.

## Recursive boundaries

New parents record actual named children in the implementation ledger.
Fixed policy-key rendering is one representation operation.
Exact handle-field decoding couples existing varint decoding with full-field
consumption. Owned-value copy couples sizing and copy while the source pin lives.
Opening, metadata selection/loading, and block lookup are compositions, not
blanket complex leaves or short-function exemptions.

Existing read/checksum/codec primitives, ValidateIndex's visitor internals,
ReadDataBlock/cache admission, iterator direction/positioning, and other
production functions remain subsequent recursive-review frontiers.
This batch does not certify the entire table file or production tree.

## Characterization before extraction

Retain all TableTest cases: pinned LevelDB golden files/compression, snapshots,
custom comparator, empty/multiblock tables, filter bypass/absence, cache hits,
fill-cache/charge/eviction, malformed index/meta/filter, short/oversized/I/O
reads, mapped backing, lazy data corruption, concurrency and ordered-model tests.
Also include TableCacheTest and DumpFileTest as coupled public-operation suites.

Strengthen `ReportsReadFailuresWhileOpening` to require exact injected Io
message and read count 1/2/3/4 at footer/index/metaindex/filter failure,
proving stop at the failed stage without changing the fixture's concurrent-read
contract or adding an unsafe shared trace vector.

Add exact policy-selection boundaries: empty metaindex, only a differently named
policy, matching malformed handle, and malformed filter contents. Assert successful
absence versus original exact corruption, and 3 versus 4 reads where applicable.
Keep index validation before filtering; characterize malformed index winning over
a configured later-read failure, and verify filtering disabled skips bad metaindex.

Directly supply a sentinel value vector to Get for index miss, data miss,
user-key mismatch, deletion and malformed data-key errors. It must remain
unchanged. On Value, require exact copied bytes after all table/file/cache owners
are released, covering both copied and mapped block paths and zero-capacity
cache ownership. Do not mutate a live immutable backing file to prove ownership.
The existing TryGet fixture creates a fresh vector and does not alone establish
the unchanged-output contract.

Also freeze the no-next-block boundary directly: Get `"apq"` at maximum sequence
from unfiltered `ThreeBlockTable`, require Missing and unchanged sentinel with
exactly one extra read, and inject failure on the second extra read. Missing and
sentinel checks alone could pass even if a new implementation incorrectly read
another block and still returned Missing.

These cases characterize unchanged behavior and must pass on original code,
not fabricated red tests. Run the same combined
`^(TableTest|TableCacheTest|DumpFileTest)\.` selection before and after, verify
exact JUnit case sets without failures/errors/skips, and retain per-slice runs.
Run warnings-as-errors build, formatting, documentation and whitespace checks.
Independent implementation review and authoritative GCC13 100% changed-branch
coverage plus all applicable exact-head CI are required before merge.

## Independent design review

Independent source-backed review identified the no-next-block characterization
gap above; its explicit read-count/fault-injection requirement is included.
It otherwise found the proposed boundaries and test suites consistent with
source. This was static design review, not execution evidence, allocation or
coverage verification, whole-file certification, or GitHub approval.
