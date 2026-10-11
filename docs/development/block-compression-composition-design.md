# Block compression codec composition

## Status and scope

Design accepted in #183 for F03 from the
[production composition plan](production-function-composition-plan.md).
Design publication/merge precedes production implementation.

Scope is codec-specific adapters and public selection in
`src/table/compression.cc`, with directly related compression tests.
Do not change codec libraries, formats, size limits, validation policy,
worthwhile-size threshold, public signatures, writers/readers, or benchmarks.

The writer implementation #182 touches none of these source/test paths.
This design adds only this document and does not change the shared plan ledger.
There is no scheduling-only dependency on #182. Implementation requires this
design's own independent review and merge.

## Existing compression contract

TryCompressBlock clears scratch first, including empty, None and unknown
requests. An empty raw input returns false before codec dispatch.
False means the caller stores raw bytes, and scratch is empty on return.
Do not alias raw input with scratch or change the existing borrowing contract.

Snappy obtains MaxCompressedLength, admits that capacity using PrepareOutput,
compresses into the resized buffer, then shrinks it to the reported size.
An unrepresentable bound returns false without allocating that bound.

Zstd obtains compressBound, checks its error sentinel before attempting output
preparation, compresses at the requested level, checks the compression result,
then shrinks to the reported size. Preserve short-circuit ordering and both
failure-side scratch clears. No option validation is added to this hot path:
existing callers validate configuration at their admission boundary.

Only successful codec production reaches the common worthwhile-size test.
Its strict `compressed < raw - raw / 8` policy is unchanged, including rounding
for small inputs and strict rejection at equality. A rejected representation
clears scratch. Successful calls retain vector capacity for reuse.
No new catches, allocation fallback, retries, or output copies are introduced.

## Existing decompression contract

Snappy first validates its declared length, prepares a bounded owned output,
and decodes its contents. Error order and exact Corruption messages remain
invalid length, too large, then invalid contents. The valid empty Snappy stream
uses the existing local char as a nonnull destination; never pass an empty
vector's possibly null data pointer to RawUncompress.

Zstd obtains frame content size before constructing the output vector, rejects
zero, unknown/error sentinels and unrepresentable sizes before allocation,
then decodes and checks both the codec error result and exact output length.
The two size sentinels remain covered by the original bound/static assertion.
Malformed size takes priority over malformed payload.

None and unknown decompression types retain the original invalid-type error.
Stored spans remain borrowed only during the call; successful output owns its
bytes. Preserve all error text, output vectors and library call ordering.

## Intended responsibilities

Private file-local adapters:

| Function | Children and stopping boundary |
|---|---|
| `CompressSnappyBlock` | Composes Snappy's native bound primitive, shared bounded output preparation, native raw compression, and final output sizing; returns codec-production success without applying savings policy. |
| `CompressZstdBlock` | Composes Zstd bound admission, shared output preparation, native frame compression/result validation, and final sizing; preserves its original failure clears. |
| `DecompressSnappyBlock` | Composes native length admission, shared bounded output preparation, empty-output destination selection, native decoding/result validation, and owning return. |
| `DecompressZstdBlock` | Composes native frame size admission, bounded owned allocation, native decoding/error-and-length validation, and owning return. |

TryCompressBlock remains the orchestrator for scratch reset, empty-input
admission, requested-codec dispatch and common savings policy. Each adapter
failure returns immediately; failure must not reach the common threshold test.
None/unknown requests never invoke an adapter.

DecompressBlock becomes only codec selection and the unchanged unsupported-type
error. Return adapter Results directly, without extra Error copying or vector
materialization. Use nonconst local Results only if a new caller layer needs
to propagate one; do not accidentally copy an error through const access.

PrepareOutput remains the shared output-capacity operation; reuse it only where
the original branches did. Zstd decoding still rejects zero/unknown sizes and
resizes directly, rather than changing it to Snappy's different size contract.
CorruptCompression remains the existing error-construction primitive.

The adapters are compositions, not certified monolithic leaves. Native codec
operations are existing library primitives; range checks and result rejection
are their adjacent admission/verification conditions. Vector resizing and
empty-destination selection belong to the storage operation they make valid.
Do not add pure forwarding wrappers around individual codec calls, a codec
interface/framework, extra buffers, generic error adapters, or shared validation
that erases the distinct Snappy/Zstd contracts.

The implementation ledger must list every new/changed function and its children
or semantic stopping reason. ValidateCompressionOptions, representation
primitives, and broader table/block/writer internals remain separate
recursive-review frontiers, not whole-file certification.

## Characterization before extraction

Retain strict savings threshold tests, deterministic round trips, independent
raw Snappy/sized Zstd golden streams, empty Snappy success versus empty Zstd
rejection, malformed/unbounded data, scratch reuse and unsupported requests.
Retain the existing separate executable that replaces only used codec symbols
for real unrepresentable/error/mismatched-result branches.

Strengthen exact error text at the public DecompressBlock boundary for invalid
Snappy length, too-large declared Snappy output, invalid Snappy contents,
invalid Zstd size, invalid Zstd contents, and unsupported codec.
Characterize mixed size/payload faults so invalid size wins.

Use the existing codec-result fixture to record used codec calls, not production
hooks. Assert bound failures never call compression; invalid declared sizes
never call decompression; valid compressed output at exact savings equality is
cleared and one-byte-better output is accepted. Begin rejected calls with
nonempty scratch to prove reset, and retain real-codec capacity reuse.
Exercise both Snappy and Zstd at equality and one byte below. Admit a bound large
enough for each mock result; assert exactly one corresponding bound/compression
call and no calls to the other codec. Independently preseed scratch for each
rejected call. Mock threshold tests prove calls, acceptance and sizes, not byte
correctness; the real-codec round trips and independent goldens prove bytes.
Do not return impossible compressed sizes greater than a mock's admitted bound.
Only this isolated test executable uses the mock fixture, not concurrent
production code.

Run original and refactored BlockCompressionTest and CompressionFailureTest,
including the separate executable's cases. Add caller-level regression evidence
using the existing table writer/reader compression cases without modifying
their implementation. Record exact JUnit case-set parity and no failures,
errors or skips. Test count is evidence, not coverage policy.

Use existing warnings-as-errors builds, formatting, documentation and whitespace
checks. No dependency installation is required for this extraction.
Authoritative changed-branch coverage remains exact-head remote CI; do not
change thresholds, exclusions, or production reachability for an instrumentation
artifact.

## Review and delivery

Independent design review precedes design merge. Implementation uses small
characterization, compression-adapter and decompression-adapter commits in one
coherent PR. Independent combined implementation review precedes publication;
all applicable checks on the exact published head precede normal merge.

Allocation failures are not newly injected or converted into compression
fallbacks. Preserve normal exception propagation and document evidence limits.
This is one bounded codec-selection batch, not completion of the production-wide
recursive composition audit.

Independent design review identified missing explicit positive native-call
evidence for both codec threshold paths. The characterization above now requires
both paths, their corresponding call counts and independent scratch preseeding.
No other design issues were reported in the bounded static review; no tests or
builds were executed by the reviewer.

## Implementation record

Implementation starts from merged #183, with a characterization commit before
separate compression and decompression extraction commits.

### Responsibility ledger

| Function | Composition or stopping boundary |
|---|---|
| `CompressSnappyBlock` | Composes native bound calculation, shared PrepareOutput, native raw compression, and output sizing; does not apply savings policy. |
| `CompressZstdBlock` | Composes checked native bound, shared PrepareOutput, native compression/result rejection, and output sizing, retaining both original failure clears. |
| `TryCompressBlock` | Composes scratch reset/empty admission, requested-codec dispatch, and the unchanged common strict savings test with rejected-output clear. |
| `DecompressSnappyBlock` | Composes native length admission, shared bounded allocation, valid empty-output destination selection, native contents validation, and owning return. |
| `DecompressZstdBlock` | Composes declared frame-size admission before allocation, native decode/error-and-length verification, and owning return. |
| `DecompressBlock` | Composes codec dispatch and the original invalid-type error; returns child Results directly without another vector or Error copy. |

The adapters use existing codec and storage primitives rather than new
forwarding wrappers. No codec settings, limits, thresholds, API, exclusions,
caller implementations, or exception policy changed. Existing option validation
and broader recursive-review frontiers remain open.

### Characterization and local evidence

Three new isolated-codec cases and strengthened existing cases pass on original
production before extraction. Mock results now record native call counts.
Both codec threshold paths prove one matching bound/compression call, no other
codec calls, equality rejection and one-byte-better acceptance, with individually
preseeded scratch and representable admitted bounds. Size/length faults reject
before native content decoding and retain exact Corruption text.
Real-codec round trips, independent golden streams, empty-output semantics,
capacity reuse, and table/block caller regressions remain unchanged.

The compression slice passes all 14 real/mock codec cases. The combined codec,
stored-block, table-writer and table-reader selection passes the same 77 cases
before and after extraction, with exact JUnit case-set parity and no failures,
errors or skips. The warnings-as-errors Debug build and formatting,
documentation and whitespace checks pass. Independent combined implementation
review found no significant issues in the source/test diff and this record.
Review is static and does not execute tests or certify the whole production
tree; the main agent separately verifies execution evidence.
These checks do not inject allocation failures or replace authoritative
published-head remote coverage.
