# Checked batch and WAL record composition

## Status and scope

Design accepted in #185 for F01/F02 in the
[production composition plan](production-function-composition-plan.md).
No production implementation was included in the design PR. The subsequent
extraction is tracked below.

Scope is the checked record proof in `src/format/write_batch.cc` and physical
fragment bounds/checksum composition in `src/format/wal_format.cc`.
Do not change trusted batch iteration/mutation, sequence-range policy,
fragmentation, WAL I/O recovery orchestration, byte codecs or public API.
Benchmarks are excluded.

The codec implementation #184 and writer implementation #182 change none of
these production/header/test paths. This design adds only its own document and
does not edit the shared ledger; neither PR is a scheduling prerequisite.
Implementation requires this design's own reviewed merge.

## F01: complete checked batch proof

Open first rejects a short header, then decodes sequence and count, validates
exactly that many records and rejects trailing bytes. Only complete success
constructs a reader over the original unchanged borrowed record span.
The codec deliberately does not enforce sequence exhaustion; callers own that
policy. OpenTrusted and Next retain their existing invariant-based decoding.
The current callers prove the 56-bit interval at group-commit and WAL-recovery
admission, including the empty successor token; see
[ADR-0093](../adr/0093-reference-write-protocol-alignment.md).

Extract file-local `ValidateBatchRecord(ByteView& remaining)` returning Status:

1. Reject absent bytes with the original count-exceeds-records error.
2. Consume the kind byte, then reject an unknown kind before decoding lengths.
3. Consume the checked length-prefixed key.
4. Only for Value, consume the checked length-prefixed value.

Preserve exact errors and first-error order. The key/value length failure keeps
its original Error copy; the new outer validation layers move that error.
Partial advancement on failure is confined to the private proof cursor.
Never consume a value field for Deletion.

Extract `ValidateBatchRecords(ByteView records, std::uint32_t count)` returning
Status. It owns the local proof cursor, iterates exactly count times through
ValidateBatchRecord, immediately propagates failure, and finally rejects any
remaining bytes. Zero-count/nonempty input is a trailing-byte error; a malformed
earlier record beats any later record/trailing fault.

Open composes header admission/decoding, checked proof and original construction.
The helper never retains bytes or mutates backing data. Use explicit nonconst
Status locals and move failed errors into the parent Result; do not introduce
another recoverable scan, factory framework or success-shaped fallback.

ValidateBatchRecord is a composition of kind admission and existing checked
length-field primitives. Tag presence/validity and conditional value selection
belong to this one record grammar, not new one-statement wrappers.
ValidateBatchRecords composes the counted proof and final full-consumption test.
Unchanged length codecs and trusted iteration are separate recursive frontiers.

## F02: physical WAL admission and verification

Preserve current failure priority and recovery metadata:

| Stage | Result |
|---|---|
| Header shorter than seven bytes | TruncatedHeader, DropBlock, encoded_size zero |
| Declared payload beyond block capacity | PayloadTooLarge, DropBlock, encoded_size zero |
| Input shorter than complete declared record | TruncatedPayload, DropBlock, encoded_size zero |
| Zero type with zero payload | EndOfBlock without checksum validation; consumes seven bytes |
| Enabled checksum mismatch | ChecksumMismatch, DropBlock, encoded_size zero |
| Complete unknown type after optional checksum | UnknownType, SkipPhysicalRecord, complete encoded size |
| Known fragment type | Fragment borrowing exactly the declared payload and complete encoded size |

Trailing bytes after one complete physical record are allowed, not part of its
payload. Nonempty Zero is unknown, not an end marker. An unknown type with a bad
checksum must drop the block rather than skip its untrusted declared boundary;
disabled checksum verification does not disable structural/type checks.

Extract file-local `ReadWalPayloadSize(ByteView encoded)` returning
`std::expected<std::size_t, WalDecodeError>`. It composes header admission,
the original two-byte size decode, block-capacity admission and full-record
bounds. It does not inspect type/checksum or build a borrowed outcome.
The parent moves a failed WalDecodeError without another Error copy.

Change private DecodeFailure to return `std::unexpected<WalDecodeError>`,
preserving its existing aggregate fields, message, failure/action and size
defaults. Both size admission and final outcome decoding can then reuse this
exact error constructor without a second error representation or converting
one incompatible successful expected type into another.
No public result alias/signature changes.

Extract `WalChecksumMatches(ByteView encoded, ByteView payload)` as one native
checksum verification operation. It composes existing stored-CRC decoding and
unmasking with the unchanged type-plus-payload CRC primitives and comparison.
Its caller has already proved bounds; do not add repeated runtime checks.
Keep the type-byte local alive during CRC calculation and reuse existing
DecodeStoredCrc, not another little-endian decoder.

DecodeWalFragment composes size admission, type decode/zero-marker handling,
exact payload borrowing, optional checksum verification, type admission and
original outcome construction. Zero-marker success stays before any checksum
call; optional verification is not hidden in the checksum primitive.
All first-error order and recovery choices remain visible in the parent.

ReadWalPayloadSize is a bounds/field composition. WalChecksumMatches is one
coupled checksum predicate using library primitives. DecodeFailure is one
failure-envelope grammar; its defaults are semantic, not fallback success.
Existing fragmenter/cursor transitions, WAL reader policy, checked byte codecs
and other definitions remain subsequent recursive-review frontiers.

## Ownership, errors and coverage

Both public operations continue to borrow immutable caller-owned bytes.
Successful batch readers and WAL payloads must refer to the original backing
allocation; helpers only consume/copy span descriptors, never backing buffers.
No input mutation, owning parse tree, output-buffer layer or exception catch.

Preserve errors at original construction/copy boundaries and move through new
layers. A WalDecodeError includes its original Error and recovery tuple; do not
propagate only its message/code or reset encoded_size for trusted unknown types.
All aggregate constructions stay in their own declarations where required by
existing GCC13 coverage conventions. Do not add exclusions or alter thresholds.

## Characterization before production edits

Use public Open/Next and DecodeWalFragment, not private-helper tests.
Retain independent golden bytes, multi-byte lengths, borrowed entries/sequence
assignment, repeated exhausted Next, exact count/trailing guards, every fragment
type, maximal physical payload, zero markers and checksum-off behavior.

Strengthen exact Corruption messages for short batch header, exhausted count,
unknown kind, malformed key/value length and trailing bytes. Add mixed faults
proving kind-before-length, key-before-value and earlier-record-before-later
faults, including deletion with leftover bytes. Do not make every corruption
share a generic new error or add sequence policy to accommodate a test.

Strengthen all WAL failures to assert the entire tuple: Error code/message,
failure kind, recovery action and encoded_size. Combine impossible/truncated
payload with bad checksum/unknown type to prove bounds priority. Assert unknown
type with disabled checksum skips a complete declared record, while nonempty
Zero with bad enabled checksum fails checksum before unknown-type admission.
Retain empty Zero with arbitrary CRC as EndOfBlock and trailing bytes ignored
after one decoded fragment, checking borrowed payload pointer and size.

Do not mutate backing bytes while a successful borrowed reader/payload is in
use. Construct independent malformed inputs before opening them.
If another implementation boundary requires evidence, first demonstrate that
characterization on unchanged production.

Run WriteBatchTest/WriteBatchReaderTest/WalFragmenterTest/WalDecoderTest and the
existing WalReaderTest/WalWriterTest caller cases before/after, recording exact
JUnit parity with no failures/errors/skips. Verify actual discovered caller
suite names; an empty selection is an error. Use the existing warnings-as-errors
build, formatting/documentation/whitespace checks and exact-head remote CI.
No allocation-failure injection or whole-file completion claim.

## Review and delivery

Independent design review precedes design publication/merge. Implementation
uses small characterization, batch-proof and WAL-admission/checksum commits
for one coherent format-boundary PR. Independent combined implementation review
precedes publication; normal merge requires all applicable exact-head checks.

Every new/changed function receives a responsibility-ledger row. This batch
does not certify trusted paths, fragmentation or whole-production recursion.

Independent bounded design review found no significant issues in these
contracts and proposed extraction boundaries. It did not execute tests or
builds, verify remote CI, or certify implementation or whole-file completion.

## Implementation record

Implementation follows merged #185. Separate topic commits characterize the
unchanged public operations, extract the batch proof, and compose WAL payload
admission/checksum verification.

### Responsibility ledger

| Function | Composition or stopping boundary |
|---|---|
| `ValidateBatchRecord` | Composes byte presence/kind admission with existing checked key and conditional value length-field primitives; retains each original field-error copy. |
| `ValidateBatchRecords` | Owns the proof cursor and composes counted per-record validation with final full-consumption admission; moves failed errors through its new layer. |
| `WriteBatchReader::Open` | Composes original header admission/decoding, complete record proof and borrowed reader construction without another trusted scan. |
| `DecodeFailure` | One failure-envelope grammar retaining original Error, failure/action and default/trusted encoded size; unexpected return supports both result types without copying the envelope. |
| `ReadWalPayloadSize` | Composes header admission, original two-byte field decode, capacity admission and complete physical-record bounds. |
| `WalChecksumMatches` | One coupled type-plus-payload checksum predicate using existing stored-CRC/unmask/CRC primitives under proved bounds. |
| `DecodeWalFragment` | Composes size proof, zero-marker handling, exact borrowed payload, optional checksum verification, type admission and original outcome construction. |

Trusted batch iteration/mutation, unchecked sequence-header compatibility,
fragmentation and WAL reader recovery state transitions are unchanged.
No byte ownership, error text/recovery metadata, public API, coverage exclusions,
thresholds or exception policy changed. Broader named recursive frontiers
remain open; neither file is wholly certified by this extraction.

### Characterization and local evidence

Four new public-operation cases pass on unchanged production before extraction.
Batch mixed-field/record faults preserve kind/key/value/count/trailing priority.
WAL mixed structural/checksum/type faults preserve the full error tuple even
with verification disabled; nonempty Zero stays unknown rather than an end
marker. A complete block-capacity payload retains its original borrowed pointer.
Existing corruption cases now assert exact messages and all recovery fields.
Golden records, borrowed iteration, zero markers and caller recovery cases stay
in the selection.

The batch-proof slice passes all 17 batch cases. The combined batch/fragmenter/
decoder/reader/writer selection passes the same 58 cases before and after
extraction, with exact JUnit case-set parity and no failures, errors or skips.
The warnings-as-errors Debug build and formatting, documentation and whitespace
checks pass. Independent combined implementation review found no significant
issues in the source/test diff and this record. The reviewer did not execute
tests; the main agent separately verified the execution evidence.
No allocation-failure injection or authoritative remote coverage is claimed by
these local checks.
