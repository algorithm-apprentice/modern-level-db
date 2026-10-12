# Table Read, Builder and Compression Lifecycle Completion

## Scope and delivery

Design-first completion batch for table/table.cc/.h, table_builder.cc/.h and
compression.cc/.h.
Source baseline: `d3d9d7b6ec6e61f838100e36fd206bfb3876bb13`.
Prior deliveries cover table open/filter/Get #180, writer tail/metadata #182,
compression codec selection #184, key/block/cache lifecycles through #261.

This batch extracts data-block cache representation/admission and the remaining
TableBuilder AddValid stages, then gives every current Table/TableBuilder/
compression definition and table lambda a bounded semantic disposition.
Keep reviewed design, original-source characterization and two production
topics as local commits, then publish one non-draft PR.
The design and original characterization are accepted; both production topics
are implemented locally. Integrated review is complete; delivery remains
pending.

Changed production paths are table.cc/.h and table_builder.cc/.h.
compression.cc/.h are inspected unchanged. No public API, table bytes, read
order, cache identity, fill-cache policy, compression choice, sticky error,
append/sync/close order, key ordering, allocation strategy, benchmark, runtime
assert, diagnostic conditional, coverage exclusion or threshold change.

## Data-block cache representation and admission

Add file-local BlockCacheKey(cache_id, block_offset), returning the exact
16-byte fixed64 cache-id then fixed64 offset representation. ReadDataBlock calls
it only when block_cache_ is nonnull, preserving the no-cache physical work.
The local key owner remains live through lookup and optional insertion.

Add CanCacheDataBlock(cache, options, block), returning true only for a present
cache, fill_cache true and an owned/cacheable decoded Block. This is the one
cache-admission policy; it does not inspect capacity/charge or allocate.

ReadDataBlock retains parent order:

1. If a cache exists, build key and return a found handle.
2. Read/decode the block.
3. Reject an empty data block.
4. Move it into one unique const owner.
5. If admitted, insert with decoded size charge and return the cache Pin.
6. Otherwise return direct ownership.

An insertion rejected by capacity/charge still returns a live uncached Pin
through the existing trusted cache contract. Borrowed mapped blocks are not
inserted, so the table/file owner remains their lifetime. No second read,
eviction, key hash policy or error translation.

## TableBuilder entry admission and data-block boundary

Add AddFilterKey(key), returning true when no filter exists, otherwise stripping
the trusted internal-key trailer and passing the user key to FilterBlockBuilder
through Record. A filter overflow installs first_error_ and prevents all later
entry/index/data mutation.

Add FinishPendingIndexEntry(next_key): if a data block handle is pending,
shorten last_key_ against next_key and call existing AddPendingIndexEntry.
No work/callback when no pending entry.

Add AppendDataEntry(key, value), preserving exact parent mutation order:
assign last_key_, increment entry_count_, then call data_block_.Add. A throwing
BlockBuilder Add therefore observes the same already-advanced parent state and
still requires abandoning/resetting the builder per existing contract.

Add FlushDataBlockIfNeeded(), returning true below block_size_. At/above the
boundary call WriteDataBlock and report whether first_error_ remains absent.
WriteDataBlock retains block Finish/write/reset/pending-handle/filter-range
ordering.

AddValid composes filter admission, pending-index completion, data-entry commit
and boundary flush. On false children it returns the existing copied FirstError.
Checked Add and trusted AddTrusted retain finished/sticky/key-order policy.

## Selected eight-definition ledger

| Definition | Composition or stopping boundary |
| --- | --- |
| BlockCacheKey | New fixed cache-id/offset representation leaf |
| CanCacheDataBlock | New cache/fill/ownership admission policy |
| Table::ReadDataBlock | Cache lookup, physical load, empty proof, owner/admission parent |
| TableBuilder::AddFilterKey | New optional user-key filter admission |
| TableBuilder::FinishPendingIndexEntry | New pending separator/index publication |
| TableBuilder::AppendDataEntry | New key/count/block mutation transaction |
| TableBuilder::FlushDataBlockIfNeeded | New size-boundary write/status decision |
| TableBuilder::AddValid | Filter/index/data/flush orchestration |

## Forty-six inspected unchanged Table definitions

| Definition | Responsibility retained |
| --- | --- |
| ReadExactly | Repeated short random reads with oversized/EOF proof |
| InBlockRegion | Footer-bounded offset/size/trailer admission |
| ReadStoredBlock | View-or-copy physical read then stored decode |
| ReadBlock | Stored contents then Block layout creation |
| IndexHandle | Trusted complete block-handle decode |
| ValidateIndex | Physical entries plus key/handle/nonoverlap proof |
| ReadFooter | Exact footer read then decode |
| ReadValidatedIndex | Block read then index semantic proof |
| ReadValidatedMetaindex | Block read then physical entry/restart proof |
| FilterMetaindexKey | Fixed filter policy key grammar |
| DecodeFilterHandle | Exact one-handle/full-field proof |
| FindFilterHandle | Validated seek/name match/optional handle |
| ReadFilterBlock | Stored-block read then filter reader creation |
| ReadFilter | Meta acquisition, optional selection, reader publication |
| Table::Open | File/footer/index/optional filter/cache identity/owner parent |
| deleted comparator-rvalue Open | Reject temporary comparator |
| deleted Table copy constructor | Preserve unique file/index/filter owner |
| deleted Table copy assignment | Preserve unique file/index/filter owner |
| deleted Table move constructor | Keep iterator/backing addresses stable |
| deleted Table move assignment | Keep iterator/backing addresses stable |
| defaulted Table destructor | Release file/index/filter owners |
| Table private constructor | `noexcept` move file/index/filter owners while borrowing comparator/cache pointers in declared member order |
| Table::Get | Index/filter/data-block routing and pinned lookup |
| Table::LookupDataBlock | Data seek/key equality/deletion/owned copy |
| BlockReference cache constructor | Own one cache Pin |
| BlockReference direct constructor | Own one decoded Block |
| BlockReference::block | Select pinned or directly owned Block |
| Iterator constructor | Borrow table/options and construct index iterator |
| deleted rvalue-Table Iterator constructor | Reject temporary table |
| deleted const-rvalue-Table Iterator constructor | Reject temporary table |
| deleted Iterator copy constructor | Preserve one mutable position |
| deleted Iterator copy assignment | Preserve one mutable position |
| deleted Iterator move constructor | Keep table/block borrows stable |
| deleted Iterator move assignment | Keep table/block borrows stable |
| defaulted Iterator destructor | Release block reference/data/index buffers |
| Iterator::valid | Test positioned data iterator presence |
| Iterator::key | Borrow current data key |
| Iterator::value | Borrow current data value |
| Iterator::SeekToFirst | Position index first then enter first data edge |
| Iterator::SeekToLast | Position index last then enter last data edge |
| Iterator::Seek | Target proof/index seek/load/data seek/next-block boundary |
| Iterator::Next | Advance data then index/first edge |
| Iterator::Prev | Retreat data then index/last edge |
| Iterator::LoadBlock | Clear prior owner, load block, construct data iterator |
| Iterator::EnterBlock | Load, position selected edge, require reachable entry |
| Iterator::Fail | Clear data/block owners and return moved Error |

## Twenty-four inspected unchanged TableBuilder definitions

| Definition | Responsibility retained |
| --- | --- |
| TableBuilder constructor | Install owners/options/builders and initial validation/filter key |
| deleted comparator-rvalue constructor | Reject temporary comparator |
| deleted builder copy constructor | Preserve unique file/buffers |
| deleted builder copy assignment | Preserve unique file/buffers |
| deleted builder move constructor | Keep borrowed dependencies/Finish state stable |
| deleted builder move assignment | Keep borrowed dependencies/Finish state stable |
| defaulted builder destructor | Release file/buffers |
| TableBuilder::Add | Finished/sticky/internal-key/order admission then AddValid |
| TableBuilder::AddTrusted | Finished/sticky admission then AddValid |
| TableBuilder::Finish | Terminal gate, tail, Close and first-error priority |
| TableBuilder::Record | Latch first failed Status only |
| TableBuilder::FirstError | Copy retained Error into failed Status |
| AddPendingIndexEntry | Encode pending handle and publish index entry |
| WriteDataBlock | Finish/write/reset data block, mark pending, advance filter range |
| WriteBlock | Optional worthwhile compression then raw write and scratch clear |
| WriteRawBlock | Handle publication, body/trailer append and size commit |
| AddFilterIndexEntry | Finish/write filter and add metaindex handle |
| WriteMetaindex | Own local builder, filter entry, finish/write |
| AddFinalIndexEntry | Pending shortest successor and index publication |
| WriteIndex | Complete pending entry then finish/write index |
| WriteFooter | Encode/append/size then Sync |
| WriteTail | Pending data, metaindex, index, error gate, footer |
| entry_count | Read committed parent entry count |
| file_size | Read completely appended table bytes |

## Ten inspected unchanged compression definitions

| Definition | Responsibility retained |
| --- | --- |
| CorruptCompression | Construct Corruption with caller-selected literal |
| PrepareOutput | Size/max admission then vector resize |
| CompressSnappyBlock | Bound/admit/native compress/resize |
| CompressZstdBlock | Bound/error/admit/native compress/error/resize |
| DecompressSnappyBlock | Length/admit/native decode owned vector |
| DecompressZstdBlock | Frame size/admit/native decode/size equality |
| ValidateCompressionOptions | None/Snappy or Zstd level admission |
| TryCompressBlock | Scratch reset/codec selection/worthwhile policy |
| DecompressBlock | Snappy/Zstd dispatch or invalid-type Error |
| CompressionIsWorthwhile | Require at least 12.5% size saving |

## Two inspected unchanged lambda bodies

| Lambda | Responsibility retained |
| --- | --- |
| ValidateIndex visitor | Per-entry key-domain, exact handle, region and nonoverlap proof |
| ReadValidatedMetaindex no-op visitor | Request physical entry/restart validation without semantic payload work |

The eight selected/new definitions plus these 80 unchanged named rows yield
88 final named definitions, plus two lambda bodies.

## Independent design review

Independent source-backed review corrected three gaps before implementation.
CompressionFailureTest belongs to a separate executable, so both original and
final evidence must fresh-build `modern_leveldb_compression_failure_tests`.
The filter-overflow characterization now captures the first Error and requires
later AddTrusted/Finish failures to preserve code, message and source location.

The Table ledger now records the real boundary: Open owns the potentially
throwing raw allocation/publication under its existing coverage rationale; the
private constructor is noexcept and initializes borrowed comparator/cache
pointers and moved file/index/filter owners in declared member order. No
production edits or tests were performed during this bounded review.

## Original-source characterization

Existing Table tests cover copied/mapped backing, output ownership, filters,
cache hits/no-fill/identity/zero-capacity/charge rejection/pin lifetime,
compressed charge, iterators and corruption/recovery. TableBuilder tests cover
golden tables, block boundaries, filtering/compression, validation, every
append/sync/close failure, tail prefixes and metadata. Compression tests cover
codec adapters/options/worthwhile policy.

Before production edits:

1. Add a mapped-view table with a real block cache and fill_cache true. Repeated
   value lookup must succeed while cache total_charge stays zero, proving
   borrowed/noncacheable data blocks never enter the cache.
2. Strengthen `RejectsFilterBlocksLargerThan4GiB`: after seven accepted entries
   record entry_count/file_size/output bytes; the eighth filter failure and
   subsequent AddTrusted/Finish must preserve all three, copy the exact original
   Error code/message/source location, and Close exactly once.

Use public Table/TableBuilder operations, not private helper hooks. Do not mutate
live mapped backing or infer cache allocator failure.

Freshly build modern_leveldb_unit_tests, the Windows native child, and the
separate modern_leveldb_compression_failure_tests executable on unchanged
production. Freeze exact unique identities for TableTest,
TableNativeTest, TableBuilderTest, BuildTableTest, TableCacheTest,
BlockCompressionTest, CompressionFailureTest, LookupTest and DatabaseTest.
Require every suite to contribute nonzero identities.

Independently review characterization before production edits. Commit Table
cache and TableBuilder stages separately, checking exact original subsets.
Finish with full identity parity and complete GCOVR/LCOV/NOLINT annotation
multiset equality for all six production paths.

Local GCC Release warnings-as-errors should compile changed/coupled objects with
`-fno-range-for-ext-temps`. Independent integrated review must reconcile all 88
definitions/two lambdas, hot read/write ordering, cache/pin ownership, sticky
errors and prior Open/Get/Finish/compression evidence. One published exact head
must pass every hosted check, authoritative GCC 13 branch coverage and actual
Windows Debug original-name parity before normal squash delivery.

Successful tests do not prove arbitrary allocation failure, mapped backing
mutation, unsupported builder reuse, physical copy/move/destruction counts,
performance, or whole-production completion.

## Local implementation evidence

The accepted design is commit `f8c382f`. Original-source characterization is
split before production edits: `1538906` proves mapped data blocks remain
uncached; `69e050c` proves filter failure preserves builder state and exact
sticky Error copies. Independent review found no significant issue.

A fresh unchanged-production Windows Debug build included the unit/native
targets and separate compression-failure executable, freezing 154 identities:

| Suite | Cases |
| --- | ---: |
| TableTest | 39 |
| TableNativeTest | 2 |
| TableBuilderTest | 19 |
| BuildTableTest | 5 |
| TableCacheTest | 7 |
| BlockCompressionTest | 7 |
| CompressionFailureTest | 7 |
| LookupTest | 14 |
| DatabaseTest | 54 |

The production topics remain separate local commits. `99b6e74` composes the
cache key/admission and ReadDataBlock parent; its five-suite selection is the
exact original 116-identity subset. `c782208` composes TableBuilder filter/
pending-index/data/flush stages; its three-suite selection is the exact original
78-identity subset. The final integrated selection exactly matches all original
154 identities, with no failure, error, skip or duplicate.

Complete GCOVR/LCOV/NOLINT annotation multisets are unchanged across all six
production paths. The final scan reports 88 named definitions and two lambdas.
Its four table.h grammar-recovery nodes are byte-for-byte/position-identical to
the baseline scanner artifact and are not new parse failures. Local GCC Release
warnings-as-errors with `-fno-range-for-ext-temps` compiles all production,
ordinary test and separate compression-failure objects. Documentation
validation and `git diff --check` pass. Hosted GCC 13 branch coverage, Windows
native execution/name parity and normal merge remain delivery obligations.

Independent integrated review found no significant issue across the complete
diff, 88 named definitions, two lambdas, baseline grammar-recovery nodes and
separate compression-failure executable evidence. Hosted coverage, arbitrary
allocation failure, mapped backing mutation, unsupported builder reuse,
performance and unrelated production remain outside this bounded conclusion.
