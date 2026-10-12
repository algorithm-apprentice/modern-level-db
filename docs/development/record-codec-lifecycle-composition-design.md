# Trusted Batch and WAL Fragment Codec Lifecycle

## Scope and delivery

Design-first follow-up to delivered checked record validation #186, WAL reader
transitions #204, and WAL append/block progress #211.
Source baseline: `fa6cd4131733f3a4b18b4d85af7a20d6fcbd347d`.
Those batches intentionally left trusted WriteBatch iteration/mutation and
WalFragmenter::Cursor transitions open.

This larger related batch gives every current definition in
format/write_batch.cc/.h and format/wal_format.cc/.h a bounded semantic
disposition while extracting complete trusted batch record decode/emission and
incremental WAL fragment cursor stages. Keep reviewed design,
original-source characterization and two production topics as local commits,
then publish one non-draft PR.

No public API, batch/WAL bytes, sequence policy, checked validation, checksum,
recovery action, WAL I/O, sticky-error policy, allocation strategy, payload
ownership, benchmark, runtime assert, diagnostic conditional, coverage
exclusion or threshold change. write_path/recovery/wal_io are real callers but
remain unchanged.

## Existing trusted representation contracts

WriteBatchReader::Open validates complete external bytes once. OpenTrusted
borrows an EncodedWriteBatch whose mutators preserve kind/length/count
invariants. Next therefore performs no recoverable checks: it consumes exactly
one record, assigns `sequence_ + index_`, increments the index, and returns
borrowed key/value views. Deletion consumes no value field; exhausted calls
return null repeatedly without touching the cursor.

EncodedWriteBatch owns a fixed 12-byte header followed by ordered records.
Put/Delete append complete record bytes, then increment the fixed32 count.
A throwing resize leaves count unchanged. Inputs are copied and cannot alias the
private encoding. Append preserves destination sequence and source, including
self-append; empty sources do nothing. Move/reset/header access remain unchanged.

WalFragmenter::Cursor borrows the fragmenter's block offset/type CRC table and
the logical record. One cursor at a time mutates shared block offset.
Next first handles a tail shorter than one header as physical zero padding,
then computes available payload, selects Full/First/Middle/Last from begin/end,
borrows the next payload span, advances shared offset and remaining state, and
returns a header plus borrowed payload. Cursor state advances before I/O and is
not retryable after a caller write failure.

Exactly seven bytes left may emit an empty First for a nonempty record. A shorter
tail is padding. Exact block fill normalizes offset to zero. An empty logical
record is one empty Full fragment. Repeated Next after done returns null without
changing offset.

## Topic one: trusted batch record decode and complete mutation

Add file-local TrustedBatchRecord containing kind/key/value borrowed views.
Add DecodeTrustedBatchRecord(remaining), which consumes the kind, trusted key
length and conditional trusted value length in the original order. It performs
no validation, allocation, sequence arithmetic or index mutation.

WriteBatchReader::Next retains the exhausted-index gate, then invokes the child,
constructs one WriteBatchEntry with `sequence_ + index_`, increments index only
after complete decode, and returns the aggregate. Borrowed views remain backed
by the original batch; repeated exhausted calls remain inert.

Add EncodedRecordSize(kind, key, value), preserving the original uint32 length
casts and exact expression: kind byte, key varint/payload, and only for Value the
value varint/payload. No overflow guard, canonical-length policy or allocation.

Add EncodeTrustedBatchRecord(output, kind, key, value), writing kind, advancing
once, emitting the trusted key prefix/payload, and conditionally the value.
It borrows/advances a proven-sized MutableByteView and is noexcept.

AppendRecord becomes the complete private mutation: compute size, resize the
existing string, create the suffix output, emit the trusted record, then
increment count. Put/Delete delegate once with Value or Deletion. Count remains
unchanged on resize failure; no later operation may throw after successful
resize under the trusted byte primitives. Append/SetSequence/Clear remain
separate complete batch operations.

## Topic two: incremental WAL fragment cursor stages

Add SelectFragmentType(begin, end), preserving the exact four-way priority:
Full, First, Last, Middle.

Add Cursor::TakePadding(), computing bytes left in the current block. When it is
at least WalHeaderSize return zero without mutation; otherwise return the
complete tail and reset shared offset to zero. No bytes are written here.

Add Cursor::FragmentPayloadSize(), computing available bytes after the header
from the normalized offset and returning `min(remaining_.size(), available)`.
The exactly-seven-byte case therefore returns zero.

Add Cursor::Advance(fragment_size, end), updating shared block offset modulo
WalBlockSize, then remaining_, begin_ and done_ in the original order.
It does not build a header or retain the payload view.

Add BuildWalFragment(padding, type, payload, type_crc), constructing the original
aggregate with EncodeHeader and the selected precomputed type CRC. It borrows
payload and adds no owner.

Cursor::Next retains its done gate, then composes padding, fragment size/end,
type selection, payload borrowing, state advance and final aggregate
construction. Advance remains before return/header ownership escape exactly as
the original single function. TypeIndex and EncodeHeader remain existing
representation leaves.

## Selected thirteen-definition ledger

| Definition | Composition or stopping boundary |
| --- | --- |
| DecodeTrustedBatchRecord | New trusted kind/key/conditional-value cursor decode |
| WriteBatchReader::Next | Exhaustion, trusted record, sequence/index publication |
| EncodedRecordSize | New exact trusted record size expression |
| EncodeTrustedBatchRecord | New kind/key/conditional-value byte emission |
| EncodedWriteBatch::Put | Value-record adapter |
| EncodedWriteBatch::Delete | Deletion-record adapter |
| EncodedWriteBatch::AppendRecord | Resize, emit and count-commit parent |
| SelectFragmentType | New begin/end to Full/First/Last/Middle policy leaf |
| Cursor::TakePadding | New short-tail normalization transition |
| Cursor::FragmentPayloadSize | New normalized available/min payload selection |
| Cursor::Advance | New offset/remaining/begin/done state transition |
| BuildWalFragment | New header plus borrowed payload aggregate construction |
| WalFragmenter::Cursor::Next | Incremental fragment-stage orchestration |

## Forty-six inspected unchanged definitions

### WriteBatch: twenty-five

| Definition | Responsibility retained |
| --- | --- |
| ValidateBatchRecord | Checked one-record grammar proof |
| ValidateBatchRecords | Counted proof and trailing-byte admission |
| WriteBatchReader::Open | Header decode, complete checked proof, borrowed reader |
| WriteBatchReader::OpenTrusted | Borrow invariant-preserving owned batch records |
| deleted reader copy constructor | Prevent duplicated mutable cursor |
| deleted reader copy assignment | Prevent duplicated mutable cursor |
| defaulted reader move constructor | Transfer borrowed cursor state |
| defaulted reader move assignment | Transfer borrowed cursor state |
| defaulted reader destructor | Release no backing bytes |
| reader sequence accessor | Read starting sequence |
| reader count accessor | Read declared record count |
| private reader constructor | Install borrowed records/sequence/count cursor |
| default EncodedWriteBatch constructor | Install canonical zero header |
| defaulted batch copy constructor | Copy complete owned encoding |
| defaulted batch copy assignment | Copy complete owned encoding |
| batch move constructor | Swap source bytes into canonical destination/source |
| batch move assignment | Self guard via replacement construction/swap |
| defaulted batch destructor | Release owned string |
| EncodedWriteBatch::Append | Append source records and count, preserve sequence/source |
| EncodedWriteBatch::SetSequence | Encode hidden fixed64 starting sequence |
| EncodedWriteBatch::Clear | Restore canonical zero header |
| EncodedWriteBatch::sequence | Decode fixed64 header sequence |
| EncodedWriteBatch::count | Decode fixed32 header count |
| EncodedWriteBatch::encoded | Borrow complete owned bytes |
| EncodedWriteBatch::SetCount | Encode fixed32 header count |

### WAL format: twenty-one

| Definition | Responsibility retained |
| --- | --- |
| IsFragmentType | Admit Full/First/Middle/Last types |
| TypeIndex | Map admitted fragment type to CRC table index |
| EncodeHeader | Type/payload checksum, little-endian length and type bytes |
| DecodeStoredCrc | Decode little-endian stored CRC |
| DecodeFailure | Construct complete Error/failure/recovery/size envelope |
| ReadWalPayloadSize | Header/capacity/complete-payload proof |
| WalChecksumMatches | Compare stored and type-plus-payload CRC |
| WalFragmenter constructor | Normalize offset and precompute four type CRCs |
| DecodeWalFragment | Bounds, zero/checksum/type and borrowed outcome parent |
| deleted Cursor copy constructor | Keep one mutable cursor over shared offset |
| deleted Cursor copy assignment | Keep one mutable cursor over shared offset |
| deleted Cursor move constructor | Keep cursor address/state simple |
| deleted Cursor move assignment | Keep cursor address/state simple |
| defaulted Cursor destructor | Release no payload or fragmenter owner |
| private Cursor constructor | Borrow offset/CRC/payload and initialize begin/not-done |
| deleted WalFragmenter copy constructor | Preserve one shared block-offset owner |
| deleted WalFragmenter copy assignment | Preserve one shared block-offset owner |
| deleted WalFragmenter move constructor | Preserve cursor back-pointer stability |
| deleted WalFragmenter move assignment | Preserve cursor back-pointer stability |
| WalFragmenter::Fragment | Create one borrowing cursor |
| WalFragmenter::block_offset | Read current normalized shared offset |

The 13 selected/new definitions plus these 46 rows yield 59 post-change named
definitions and cover every current syntax-level function in both source/header
pairs. There are no current lambdas. The previously delivered checked-validation
definitions are included above as inspected unchanged context. WAL I/O
definitions in wal_io.cc/.h remain a separately delivered external
caller/frontier outside this 59-definition ledger.

## Independent design review

Independent source-backed review corrected two documentation inconsistencies
before implementation. The WAL cursor characterization now expresses the
maximum payload only through public constants, keeping MaximumWalPayload
file-local. The complete-definition statement now distinguishes included
unchanged checked-validation functions from excluded WAL I/O callers.

No tests, builds or production edits were performed during this bounded review.
Implementation, hosted coverage and physical allocation/copy behavior remain
future evidence.

## Original-source characterization

Existing tests already cover golden Put/Delete bytes, empty/binary/multibyte
lengths, borrowed checked/trusted iteration, append/self-append, move/copy,
clear, malformed priority, noncanonical varints, fragment golden records,
short-tail padding, exact seven-byte boundary, exact block fill, borrowed
large fragments, all decoded types, error tuples and maximal payload.

Before production edits, add three cases:

1. A literal record-boundary matrix emits Put/Delete keys and values of lengths
   127/128 with binary edge bytes, verifies exact prefix bytes/count after every
   mutation, then uses OpenTrusted to prove one-at-a-time borrowed key/value
   cursor positions and deletion's absent value.
2. A trusted mixed Delete-then-Put batch uses nonzero starting sequence and
   repeated exhausted Next calls, proving conditional value consumption,
   sequence/index commit only after each complete record, and unchanged backing.
3. A fragment cursor starts with exactly one header left and a logical record
   of `(WalBlockSize - WalHeaderSize) + 1U` bytes. Observe empty First and
   offset zero, full Middle and offset zero, one-byte Last and offset eight,
   exact payload pointer progression, then repeated done calls with no offset
   change.

Use public format operations, not private helper hooks. Literal expected prefixes
must not be produced with EncodedRecordSize/EncodeTrustedBatchRecord.
Physical headers may use the existing independent test encoder for complete
byte comparisons. Do not infer retry after cursor advancement.

Freshly build modern_leveldb_unit_tests and the Windows native child on
unchanged production. Freeze exact unique identities for WriteBatchTest,
WriteBatchReaderTest, WalFormatTest, WalFragmenterTest, WalDecoderTest,
InsertBatchTest, CommitGroupTest, WalWriterTest, WalReaderTest, WalIoNativeTest,
RecoveryTest and DatabaseTest, with no failure/error/skip.

Independently review new cases before production edits. Commit trusted batch and
fragment cursor topics separately, checking exact relevant original subsets.
Finish with full identity parity and complete GCOVR/LCOV/NOLINT annotation
multiset equality for all four owned production paths.

Local GCC Release warnings-as-errors should compile all changed/coupled objects
with `-fno-range-for-ext-temps`. Independent integrated review must reconcile
all 59 definitions, borrowed lifetimes, count/index commit points, cursor
state/order, checksum/header reuse and every prior checked/I/O boundary.
One published exact head must pass all hosted checks, authoritative GCC 13
branch coverage and actual Windows Debug original-name parity before normal
squash delivery.

Successful tests do not prove arbitrary allocation failure, unsupported input
aliasing, cursor retry after I/O failure, concurrent cursors, physical copy/
move/destruction counts, performance, or whole-production completion.
