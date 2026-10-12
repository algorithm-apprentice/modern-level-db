# Block Representation, Builder and Iterator Lifecycle

## Scope and delivery

Design-first completion batch for table/block.cc/.h, block_builder.cc/.h and
block_format.cc/.h.
Source baseline: `486f0594835377e17f517b69c8d7e873c5133347`.
Prior deliveries cover block positioning #208, reverse positioning #241,
prefix-compressed building #245, and read-record entry/layout admission #254.

This batch closes the remaining stored-block/footer composition and gives every
unchanged Block/Iterator/BlockBuilder body a bounded semantic disposition.
Keep reviewed design, original-source characterization and the block-format
production topic as local commits, then publish one non-draft PR.
The design and corrected original characterization are accepted; both
production topics are implemented locally. Integrated review is complete;
delivery remains pending.

Changed production path is block_format.cc. Its header and all Block/
BlockBuilder source/headers are inspected but unchanged. No public API,
on-disk bytes, checksum, compression codec, restart topology, iterator
positioning, key reconstruction, ownership/cacheability, allocation strategy,
benchmark, runtime assert, diagnostic conditional, coverage exclusion or
threshold change.

## Footer handle and padding composition

DecodeFooter retains bad-magic priority before every handle/padding read.
Add DecodeFooterHandles(handles), consuming metaindex then index with existing
ConsumeBlockHandle. Preserve each exact malformed-offset/size Error copy and
leave the remaining padding span for the parent. The child returns a complete
Footer only after both handles succeed.

Add PaddingIsZero(padding), a direct byte loop returning false at the first
nonzero byte. This replaces the anonymous ranges predicate lambda and adds no
allocation/callback. DecodeFooter composes magic admission, handle decode,
zero-padding proof and complete Footer return in that order. The new outer
Result moves a failed Error and adds no extra copy/source location.

EncodeFooter, handle codecs and magic/padding wire layout remain unchanged.
Trailing handle padding remains fixed zero bytes; noncanonical varints accepted
by the scalar decoder remain accepted.

## Stored-block checksum, compression type and ownership

Add StoredBlockChecksumMatches(stored, contents_size, type), decoding the
stored trailing fixed32 and comparing it with BlockChecksum over exact contents
plus type. Its caller has already proved the five-byte trailer.

Add DecodeCompressionType(type) returning optional BlockCompression for None,
Snappy or Zstd. It constructs no Error. ValidateStoredBlock retains exact
priority and parent Error locations: short trailer, checksum mismatch, unknown
compression, then complete StoredBlock borrowed layout publication.

The owning overload retains the original vector. For None, resize away the
trailer and move the same allocation into BlockContents::Owned. For compressed,
call the existing DecompressBlock owner directly and own the returned vector.
The borrowed overload returns
BlockContents::Borrowed only for None; compressed data always becomes owned and
survives source release. Both retain the original ValidateStoredBlock Error copy
and move decompression failures from the existing child. Do not add a forwarding
wrapper around that already complete decompression owner, generic template,
variant visitor, extra backing owner or catch.

BlockContents::data/cacheable and move-only representation remain unchanged.

## Selected six-definition ledger

| Definition | Composition or stopping boundary |
| --- | --- |
| DecodeFooterHandles | New ordered metaindex/index handle decode |
| PaddingIsZero | New direct zero-padding proof leaf |
| DecodeFooter | Magic, handles, padding and Footer parent |
| StoredBlockChecksumMatches | New stored-versus-computed checksum predicate |
| DecodeCompressionType | New admitted compression representation leaf |
| ValidateStoredBlock | Trailer/checksum/type/layout proof parent |

## Fifty-six inspected unchanged Block/Iterator definitions

| Definition | Responsibility retained |
| --- | --- |
| DecodeVarint32 | Bounded five-byte uint32 varint fold |
| DecodeEntryHeader | Fast/slow ordered three-field header decode |
| HasEntryPayload | Widened key/value payload bounds proof |
| ReadRestartCount | Footer/count priority and restart-array region proof |
| StringBytes | Borrow string storage as bytes |
| Block::DecodeEntry | Header/payload proof and atomic borrowed aggregate publication |
| Block::ReadEntry | Offset decode plus previous-key shared bound |
| Block::ReconstructKey | Resize shared prefix then append suffix |
| Block::ValidateRestartEntry | Ordered restart offset/shared-key topology transition |
| Layout::RestartPoint | Fixed32 restart offset decode |
| Block::Create(vector) | Move vector into owned BlockContents adapter |
| Block::Create(BlockContents) | Restart-count proof and owning construction |
| Block private constructor | Install contents owner before borrowed layout |
| deleted Block copy constructor | Preserve unique contents/layout owner |
| deleted Block copy assignment | Preserve unique contents/layout owner |
| defaulted Block move constructor | Move owner while copied layout remains backed |
| deleted Block move assignment | Prevent invalidating live iterator layout assumptions |
| defaulted Block destructor | Release contents owner |
| Block::empty | Restart/count/first-offset empty predicate |
| Block::size | Borrow owned/borrowed content size |
| Block::cacheable | Delegate contents ownership policy |
| Block::ValidateEntries | Ordered physical entry/restart/key/visitor validation |
| Iterator constructor | Borrow layout/comparator and initialize invalid coordinates |
| deleted rvalue-comparator Iterator constructor | Reject temporary comparator |
| deleted rvalue-Block Iterator constructor | Reject temporary Block |
| deleted const-rvalue-Block Iterator constructor | Reject temporary Block |
| deleted Iterator copy constructor | Preserve one mutable position |
| deleted Iterator copy assignment | Preserve one mutable position |
| deleted Iterator move constructor | Keep borrowed layout/comparator address simple |
| deleted Iterator move assignment | Keep borrowed layout/comparator address simple |
| defaulted Iterator destructor | Release key buffer only |
| Iterator::valid | Error-free current-offset range predicate |
| Iterator::key | Borrow reconstructed key buffer |
| Iterator::value | Borrow current block value |
| Iterator::SeekToFirst | Reset and parse first entry |
| Iterator::SeekToLast | Reset, seek last restart, scan last entry |
| Iterator::ScanToLastEntry | Parse forward until physical end |
| Iterator::Seek | Reuse/current checkpoint or restart search then lower-bound scan |
| Iterator::FindRestart | Binary search validated restart keys |
| Iterator::RestoreCheckpoint | Choose current/restart checkpoint without backward read |
| Iterator::ScanToLowerBound | Parse/compare until first admitted key |
| Iterator::Next | Parse next physical entry |
| Iterator::Prev | Previous-restart discovery then predecessor scan |
| Iterator::FindPreviousRestart | Descend restart index and seek candidate |
| Iterator::ScanToPreviousEntry | Parse from restart through predecessor |
| Iterator::BeginPositioning | Clear prior error before positioning |
| Iterator::RestartPoint | Decode/admit one restart offset |
| Iterator::RestartKey | Decode one restart entry and borrow full key |
| Iterator::SeekToRestartPoint | Publish restart coordinates/key/value |
| Iterator::ParseNextEntry | Decode next entry and compute key/physical end |
| Iterator::CommitEntry | Publish reconstructed coordinates/key/value, then validate restart advancement; corruption invalidates the published state |
| Iterator::AdvanceRestartIndex | Validate and advance crossed restart |
| Iterator::Corruption | Install first corruption and invalidate |
| Iterator::AcceptsKeySize | Enforce Internal versus Arbitrary key domain |
| Iterator::Compare | User/internal comparator dispatch |
| Iterator::Invalidate | Restore canonical invalid coordinates/views |

## Sixteen inspected unchanged BlockBuilder definitions

| Definition | Responsibility retained |
| --- | --- |
| BlockBuilder constructor | Install restart interval |
| deleted builder copy constructor | Preserve unique buffers/history |
| deleted builder copy assignment | Preserve unique buffers/history |
| deleted builder move constructor | Keep returned borrowed Finish views stable |
| deleted builder move assignment | Keep returned borrowed Finish views stable |
| defaulted builder destructor | Release buffers |
| BlockBuilder::Add | Prefix/restart/header/payload/history parent |
| BeginRestart | Append offset then reset interval count |
| AppendEntryHeader | Ordered shared/nonshared/value varints |
| AppendEntryPayload | Ordered key-delta/value byte insertion |
| CommitEntryHistory | Reconstruct prior key then increment count |
| Finish | Restart array/count and borrowed output |
| AppendRestarts | Implicit zero then stored restart offsets |
| Reset | Clear block/restart/key/count coupled state |
| CurrentSizeEstimate | Complete finished wire-size expression |
| empty | Test entry-history count |

## Eighteen inspected unchanged block-format definitions

| Definition | Responsibility retained |
| --- | --- |
| BlockChecksum | CRC contents then type and mask |
| BlockContents::Owned | Move vector owner into representation |
| BlockContents::Borrowed | Install immutable borrowed representation |
| deleted BlockContents copy constructor | Preserve unique/borrowed representation |
| deleted BlockContents copy assignment | Preserve unique/borrowed representation |
| defaulted BlockContents move constructor | Transfer representation owner/view |
| deleted BlockContents move assignment | Preserve stable representation semantics |
| defaulted BlockContents destructor | Release optional vector owner |
| private owned BlockContents constructor | Move owned bytes |
| private borrowed BlockContents constructor | Store borrowed view |
| BlockContents::data | Select borrowed view or owned vector view |
| BlockContents::cacheable | Owned versus borrowed policy |
| AppendBlockHandle | Offset then size varint emission |
| ConsumeBlockHandle | Transactional offset/size decode and caller commit |
| EncodeFooter | Ordered handles, zero padding and magic |
| EncodeBlockTrailer | Type byte and masked checksum |
| DecodeStoredBlock(vector) | Validate then retain/move None or own decompressed bytes |
| DecodeStoredBlock(ByteView) | Validate then borrow None or own decompressed bytes |

The six selected/new definitions plus these 90 unchanged rows yield 96 final
named definitions across the six files. PaddingIsZero removes the only current
lambda, so the final bounded inventory has zero lambdas. Prior delivered designs
remain evidence for their rows; this batch reconciles rather than reimplements
them.

## Independent design review

Independent source-backed review corrected two issues before implementation.
The exact selector now names the real BlockCompressionTest suite and requires
every selected suite to have nonzero identities. A proposed
DecodeCompressedContents helper was removed because DecompressBlock already
owns the complete operation and the wrapper would only rename one call.
The reconciled inventory is six selected/new plus 90 unchanged definitions,
96 final and zero lambdas. No tests, builds or production edits were performed
during this bounded review.

## Original-source characterization

Existing cases already cover block-handle boundaries/transactional failure,
footer golden/magic/handles/padding, trailer bytes/checksums, owned same-buffer
None decoding, borrowed empty distinction, every checksum byte, Snappy/Zstd,
owning/borrowed parity, unknown types, block layout/entry errors, iterator
positioning/recovery, prefix/restart builders, tables and cache pins.

Before production edits, add three cases:

1. Footer mixed-fault priority with exact messages: bad magic wins over malformed
   handles/padding; malformed metaindex wins over index/padding; malformed index
   wins over padding; valid handles with nonzero padding reports padding.
2. Stored-block mixed priority and exact messages across owning and borrowed
   overloads: short trailer, checksum mismatch before unknown type, and unknown
   type after valid checksum.
3. Decode compressed Snappy and Zstd through the ByteView overload inside a
   source scope, move BlockContents out, destroy the compressed source, then
   verify owned/cacheable decompressed bytes. This distinguishes compressed
   ownership from uncompressed borrowed backing.

Use public format operations and independent stored-block/test compression
fixtures, not private helper hooks. Do not mutate live borrowed uncompressed
storage or infer allocator failure.

Freshly build modern_leveldb_unit_tests and the Windows native child on
unchanged production. Freeze exact unique identities for BlockHandleTest,
FooterTest, BlockTrailerTest, StoredBlockTest, BlockCompressionTest, BlockTest,
BlockBuilderTest, BlockIteratorTest, TableBuilderTest, TableTest,
TableNativeTest, TableCacheTest, LookupTest and DatabaseTest.
Require every named suite to contribute at least one identity.

Independently review new cases before production edits. Commit footer and
stored-block topics separately, checking exact relevant original subsets.
Finish with full identity parity and complete GCOVR/LCOV/NOLINT annotation
multiset equality for all six production paths.

Local GCC Release warnings-as-errors should compile all changed/coupled objects
with `-fno-range-for-ext-temps`. Independent integrated review must reconcile
all 96 definitions, ownership/error-copy/source-location behavior, iterator/
builder prior evidence and removal of the padding lambda. One published exact
head must pass every hosted check, authoritative GCC 13 branch coverage and
actual Windows Debug original-name parity before normal squash delivery.

Successful tests do not prove arbitrary allocation failure, borrowed storage
mutation, unsupported iterator domains, physical copy/move/destruction counts,
performance, or whole-production completion.

## Local implementation evidence

The accepted design is commit `f6b5e2e`. Original-source characterization is
commit `5983c3d`; independent review required fatal ownership assertions before
compressed source release. Local GCC Release then found an optimized
vector::front null-dereference warning in the new corruption fixture. The
correction was proven on unchanged production as `f4a9ad4` with exact 208-name
parity and GCC object success, then normally cherry-picked as `a06e9a2`.
Integrated review later found that metaindex/index mixed faults expected the
same message and therefore did not prove decode order. The metaindex size versus
index offset correction was proven on unchanged production as `2ba086a` with
exact parity/GCC success and cherry-picked as `8637ed6`.

The corrected unchanged-production Windows Debug selection contains 208
identities:

| Suite | Cases |
| --- | ---: |
| BlockHandleTest | 4 |
| FooterTest | 6 |
| BlockTrailerTest | 2 |
| StoredBlockTest | 9 |
| BlockCompressionTest | 7 |
| BlockTest | 13 |
| BlockBuilderTest | 7 |
| BlockIteratorTest | 26 |
| TableBuilderTest | 19 |
| TableTest | 38 |
| TableNativeTest | 2 |
| TableCacheTest | 7 |
| LookupTest | 14 |
| DatabaseTest | 54 |

The production topics remain separate commits. `b455121` composes footer handle
decode and zero-padding proof; its five-suite selection is the exact original
121-identity subset. `b5ee138` composes stored checksum and compression
admission while reconciling DecodeStoredBlock overloads as unchanged; its
eight-suite selection is the exact original 133-identity subset. Corrected final
results exactly match all original 208 identities, with no failure, error, skip
or duplicate.

Complete GCOVR/LCOV/NOLINT annotation multisets are unchanged across all six
production paths. The corrected final scan reports 96 definitions, zero lambdas
and zero recovery nodes, matching the complete ledger. Local GCC Release
warnings-as-errors with `-fno-range-for-ext-temps` compiles all changed/coupled
objects. Documentation validation and `git diff --check` pass. Hosted GCC 13
branch coverage, Windows native execution/name parity and normal merge remain
delivery obligations.

Initial integrated review found the ambiguous footer-priority oracle and the
incorrect CommitEntry ledger order. After the unchanged-source correction,
final parity and focused GCC verification, a separate focused review found no
remaining significant issue. Hosted coverage, arbitrary allocation failure,
borrowed mutation, unsupported iterator domains, performance and
whole-production behavior remain outside this bounded conclusion.
