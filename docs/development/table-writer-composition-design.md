# Table writer completion and metadata composition

## Status and scope

Design proposal for T04/T05/T06 in the
[production composition plan](production-function-composition-plan.md).
No production implementation is included in the design PR.

The batch covers `TableBuilder::WriteTail` in `src/table/table_builder.cc`
and verification/metadata construction in `src/engine/build_table.cc`.
It leaves entry admission, data-block assembly, compression, raw block writes,
cache implementation, iterator algorithms, and benchmarks unchanged.

The opening/filter/lookup implementation in #180 changes none of the selected
source/header/test paths. This design adds only this document and does not edit
the shared plan ledger. Its publication has no scheduling dependency on #180.
Implementation depends on this design's own reviewed merge.

## Contracts to preserve

### Builder completion

`Finish` marks the builder finished before tail writing, skips the tail after
an earlier error, closes a nonnull file even after a tail failure, and reports
the first error. A repeated Finish or subsequent Add remains InvalidArgument.
No new catch, retry, rollback, flush, or directory sync is introduced.

`WriteTail` retains the following evaluation and state-transition order:

1. Finish/write any nonempty pending data block.
2. Construct the local metaindex using the configured restart interval.
3. Finish the optional filter and attempt its uncompressed raw block write;
   encode its handle and add its policy-key entry to the metaindex.
4. Finish/write the metaindex.
5. Shorten a pending final index key with FindShortSuccessor and add its handle.
6. Finish/write the index.
7. Stop before footer encoding if any sticky error exists.
8. Encode and append the footer; advance size only after append success.
9. Sync only after footer append success; Finish subsequently closes the file.

Earlier sticky errors suppress writes inside WriteBlock/WriteRawBlock, but do
not currently suppress subsequent filter/block Finish, metadata construction,
or pending-index state updates before step 7. Preserve those evaluations:
an extraction must not introduce an apparently helpful early return that
changes allocation, callback, or builder-state behavior after an error.
The builder cannot be reused after a thrown exception, as documented today.

All blocks preserve byte layout, handle offsets, checksums, compression choice,
pending-entry timing and file-size accounting. Filters stay uncompressed.
No-filter tables still write the same empty metaindex, and empty tables still
write the metaindex, index, footer, sync and close.

### Build verification and metadata

`BuildTable` seeks the memtable first and touches no file for empty input.
It opens a fresh-number path, borrows the first key, and invokes existing
WriteTable. WriteTable consumes trusted entries, retains the last added key,
and calls Finish even after Add fails, thereby closing and preserving the
original first error.

Only successful writing triggers `TableCache::Find(number, size)`. Its returned
handle pins the table only for verification; release it before metadata-key
decoding, as today. Keep the existing copy of a failed Find error, moving
through any newly introduced outer Result layer. Do not create another copy
or return an owning handle as a proxy for success.

After any write/verification failure, attempt exactly the existing RemoveFile,
ignore that removal error, and return the original failure. Open failure does
not remove the path. The cache key is the fresh number; do not add eviction or
directory persistence.

After verification, decode trusted smallest then largest internal keys, in that
order, and construct FileMetadata with the same number and measured file size.
Metadata owns its decoded keys before return; borrowed memtable bytes are valid
through the call, and the memtable must remain unchanged. Preserve std::optional
empty/present results and all public signatures.

## Intended responsibility decomposition

### T04: terminal blocks and pending index

Keep WriteTail as the explicit stage orchestrator. Extract private member
children:

| Function | Responsibility and children |
|---|---|
| `WriteMetaindex` | Own a local BlockBuilder, compose optional filter-entry construction with metaindex finishing/writing, and return the resulting BlockHandle by value. |
| `AddFilterIndexEntry` | When filtering exists, compose filter finish/raw write with existing encoded handle construction and metaindex insertion; no new early error gate. |
| `WriteIndex` | Compose optional terminal index-entry completion with index finishing/writing, returning the resulting handle by value. |
| `AddFinalIndexEntry` | One terminal separator operation: pending check, FindShortSuccessor and existing AddPendingIndexEntry. |

The local metaindex stays alive across filter construction and its own finish.
Block handles are initialized exactly as today; they do not imply successful
writing when a sticky error exists. Only the parent gate authorizes footer use.
The existing AddPendingIndexEntry remains shared by ordinary and terminal
separators; do not duplicate its handle representation.

### T05: footer and persistence

Extract `WriteFooter(metaindex_handle, index_handle)` as a private member:
compose existing EncodeFooter with append/size update and conditional Sync.
WriteTail retains the post-index first-error guard before calling it.
Keep the exact append success gate: a failed append neither advances file size
nor syncs. A failed Sync retains the full appended table size and first error.

Do not extract a second footer codec, generic stage runner, or single-call Sync
wrapper. Existing Record owns first-error capture, and Finish owns close.
Record/FirstError and underlying codecs are not changed by this batch.

### T06: completed table verification and metadata

Extract file-local `VerifyWrittenTable(table_cache, number, size)` returning
Status. It owns the temporary Find result, preserves its one original error
copy, and releases a successful pin on return. BuildTable moves a failed
verification status into its existing size result and retains one cleanup path.
The parent invokes the helper only when WriteTable succeeded.

Extract file-local `MakeTableMetadata(number, size, smallest, largest)` returning
FileMetadata. It composes the existing trusted key decoding, in original order,
with aggregate construction. This helper owns decoded keys; it does not borrow
the file or cache. BuildTable retains optional construction after the helper.

Existing WriteTable is a later recursive frontier: this batch does not claim
that its entry loop/error-close orchestration is a certified leaf.

## Recursive stopping boundaries

The helpers represent domain stages, not statement-count wrappers. Final index
separator completion is a coupled operation; raw byte append, footer encoding,
trusted key decoding, and standard moves remain existing representation
primitives. WriteFooter and metadata construction are compositions, not leaves.

Every new/changed function must receive an implementation-ledger row identifying
its children or semantic stopping reason. Remaining TableBuilder admission,
filter-key initialization, WriteDataBlock/WriteBlock/WriteRawBlock,
AddPendingIndexEntry, WriteTable, parser/compression/cache internals and other
production definitions remain explicit recursive-review frontiers.
This batch does not certify either entire file.

## Characterization before production changes

Use public TableBuilder and BuildTable operations, not private helper tests.
Retain pinned LevelDB golden bytes for empty filtered/unfiltered tables and the
three-block table with shortened separators. Retain trusted/checked parity,
all append-failure injections, filter size bounds, sync/close failures,
repeated Finish guards, BuildTable empty-input silence, cached readback,
every file-operation failure, and cleanup failure preserving the original error.

Strengthen the append-failure matrix before extraction to assert the exact first
error text, partial file-size accounting, Close exactly once, no Flush, no Sync
after failed Append, and terminal InvalidArgument behavior. Exercise filtered
and unfiltered, empty and pending-data tables; use a successful reference run to
derive each append's prefix size rather than assuming a new format.

Keep separate stored-byte and reported-size oracles. In this all-or-nothing
Append fixture, stored bytes equal the successful reference append prefix.
Reported file_size includes only complete body-plus-trailer blocks: a successful
body followed by trailer failure leaves it at the previous complete-block
boundary, even though stored bytes have advanced. Footer append failure leaves
reported size unchanged; footer success advances it before Sync and therefore
retains the full size after a later Sync or Close failure. Derive lengths from
the successful reference but map reported sizes to those commit boundaries;
do not change production accounting or fault semantics to match the fixture.

Record Append/Sync/Close operation order in the local writer fixture so the
tests prove footer-before-sync-before-close and no append after first failure.
This fixture is not shared by concurrent readers; no production tracing is added.
Cover combined append/close and sync/close faults with exact first-error text.

Strengthen BuildTable metadata ownership by retaining the returned metadata
after memtable and cache owners leave scope, then checking both encoded bounds.
Retain explicit close-before-open-random-access and no directory-sync ordering.
If implementation-specific behavior requires another characterization, add it
and demonstrate it on unchanged production code before extraction.

Run the combined TableBuilderTest/BuildTableTest/TableCacheTest selection before
and after, preserving exact JUnit case sets and no failures/errors/skips.
Use the existing Debug warnings-as-errors build, formatting, documentation and
whitespace checks. Authoritative changed-code/branch coverage remains the remote
CI gate, not a local MSVC claim or reason to add exclusions.

## Review and delivery

Independent design review precedes design publication/merge. Implementation
then uses separate characterization, terminal-block, footer, and metadata
commits in one coherent PR to amortize CI. Independent combined implementation
review is required before publication; fix actionable issues and validate the
affected behavior.

Green applicable checks on the exact published head are required before normal
merge. No bypass, threshold change or silent error fallback is authorized.
Final inventory/recursive audit remains production-wide follow-up work.

Independent design review identified the distinction between successful append
prefix bytes and complete-block reported size as a characterization gap.
The separate oracles above address that finding. The remaining contracts and
proposed extraction boundaries had no reported issues in the bounded static
review. This is not execution evidence, implementation review, GitHub approval,
or whole-file certification.
