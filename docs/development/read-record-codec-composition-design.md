# Read-Record Codec Composition

## Scope and delivery

Design-first follow-up to the production function composition plan.
Source baseline: `b8d49fa3c02fd03a4713fc8ad4a7f6be38b6f22c`.
Foundation #253 is delivered. This is a related, larger delivery batch:
scalar codecs, block record decoding/layout admission, and MemTable lookup.
Benchmark code and unrelated iterator algorithms are excluded.
The design and original characterization are accepted; all three production
topics are implemented locally. Integrated review is complete; delivery remains
pending.

Keep reviewable topic commits locally, then publish one non-draft PR containing
the reviewed design, original-source characterization, implementation topics,
and integrated evidence. Design review must finish before production edits;
characterization must run on the unchanged production baseline. Do not publish
a full CI run for each extraction or a separate design-only PR for this batch.
Independent future batches may run in parallel only when owned paths and
dependencies do not overlap. These three topics share coding_internal.h and
therefore belong together, not in conflicting parallel PRs.

Owned production paths: src/base/coding.cc, src/base/coding_internal.h,
src/table/block.cc, directly dependent src/table/block.h, and
src/memory/memtable.cc. Tests stay in the corresponding three existing files.
No public API, on-disk representation, validation domain, ownership model,
runtime assertions, diagnostic conditional compilation, coverage exclusions,
or coverage threshold changes.

## Shared payload and scalar cursor transactions

Three real decoders share one compatible primitive: unsigned seven-bit payload
folding at an already-proved shift. Introduce internal FoldVarintPayload,
used by checked ConsumeVarint, pointer ConsumeVarint32Trusted, and the block's
bounded DecodeVarint32. Preserve their different termination, cursor, and
failure policies. The primitive requires an unsigned result, a seven-bit
payload, and a shift within that result's width. No new runtime check.

EncodeVarintTrusted retains its parent value, continuation loop, and terminal
emission. EmitVarintByte stores and advances once. EmitVarintContinuation
composes masking/continuation emission and the subsequent seven-bit shift.
EncodeVarint admission remains before any caller bytes/cursor mutation.
Append/resize policies remain unchanged.

ConsumeVarint retains its unsigned accumulator, maximum-byte calculation,
payload/shift locals, terminal commit, and overflow error. ReadVarintByte
performs bounded byte admission and conversion; its default source_location
captures the parent call site. A mutable Result<unsigned int> transports a
failed Error by move, not an additional copied diagnostic. The original
function/file provenance is preserved, not numeric line/column positions.
Excess terminal payload bits still truncate to the unsigned width.
Continuation on the fifth/tenth byte remains overflow even if a later byte
would terminate. Any failure leaves the caller span unchanged.

ConsumeLengthPrefixed retains its staged remaining span and mutable length
Result, including the existing copied prefix-error projection. Extract only
ValidateLengthPrefixedPayload; pass the parent source location through its
default argument and move a new payload-bounds error back to the parent.
Successful value borrowing and the final caller-span commit stay in the parent.
Do not create a replacement byte owner or unify checked and trusted policies.

## Block header, payload, and restart-array admission

DecodeEntry retains its initial three-byte admission, begin pointer, and three
parent-owned uint32 length scalars. DecodeEntryHeader borrows those scalars and
cursor, selecting the original fast three-byte path or ordered bounded varint
decoders. Short-circuit at the same first failing field.
HasEntryPayload retains the uint64 sum before comparison against remaining
bytes; do not replace it with potentially overflowing uint32 arithmetic.
Only after both children succeed does the parent atomically assign the complete
DecodedEntry aggregate of borrowed spans and header size. This direct aggregate
assignment is already a publication primitive; no seven-argument wrapper.

ReadRestartCount validates the footer's presence, decodes the final fixed32,
then validates the array region, in the original priority. Its default location
captures Create(BlockContents), and the parent moves failed diagnostic ownership.
Create retains its by-value backing owner, const data and restart-count scalars,
entries_end arithmetic, and final owning Block construction.
Count zero remains accepted by Create; complete restart/entry topology remains
lazy. Do not add eager offset, shared-prefix, ordering, or entry validation.
No private DecodedEntry/Layout types leak through new public declarations.

## MemTable lookup decisions

Lookup retains the iterator, Seek, invalid-result return, and decoded entry
aggregate for the whole function. MatchesMemTableUserKey borrows the user
comparator and views, strips the trusted trailer, and performs the one final
user-key equality comparison. SkipList comparisons remain unchanged.
ClassifyMemTableEntry returns the original Deletion/empty span or Value/borrowed
span aggregate. Empty values remain values; missing and deletion remain distinct.
No new owner, comparator invocation, insertion path, eager record validation,
or noexcept contract. Some pure getter sampling may move to argument evaluation;
this is not a claim of instruction-for-instruction equality.

## Selected eighteen-definition ledger

| Definition | Disposition |
| --- | --- |
| EncodeVarintTrusted | Parent continuation orchestration and terminal emission |
| EmitVarintByte | New byte store/cursor-advance primitive |
| EmitVarintContinuation | New continuation composition, then scalar shift |
| ConsumeVarint | Parent accumulator, termination, error priority, cursor transaction |
| ReadVarintByte | New bounded scalar-byte admission/conversion |
| ConsumeLengthPrefixed | Parent staged prefix/payload/caller commit |
| ValidateLengthPrefixedPayload | New payload-bounds proof |
| FoldVarintPayload | New shared unsigned payload fold, three real callers |
| ConsumeVarint32Trusted(const byte*&) | Trusted loop delegates compatible fold only |
| DecodeVarint32 | Bounded loop delegates compatible fold only |
| Block::DecodeEntry | Header/payload orchestration and atomic aggregate publication |
| DecodeEntryHeader | New fast/slow header orchestration |
| HasEntryPayload | New widened aggregate payload-bounds proof |
| Block::Create(BlockContents) | Parent backing ownership and admitted layout construction |
| ReadRestartCount | New ordered footer/array admission |
| MemTable::Lookup | Parent search, matching, and classification orchestration |
| MatchesMemTableUserKey | New user-key equality operation |
| ClassifyMemTableEntry | New value-kind projection preserving borrowed storage |

## Thirty unchanged codec/byte definitions

This bounded disposition includes every current definition in coding.cc,
coding_internal.h, and bytes.h, plus the five proposed codec children:
39 definitions after implementation, nine selected and 30 unchanged.
Counts are ledger bookkeeping, not semantic scanner or whole-repository proof.

| Definition | Responsibility retained |
| --- | --- |
| EncodeFixed | Fixed-width little-endian lane emission |
| DecodeFixed | Fixed-width little-endian lane accumulation |
| AppendFixed | Stack encoding then vector append |
| ConsumeFixed | Fixed-width admission then decoded cursor commit |
| EncodeVarint | Output admission then trusted encoding |
| AppendVarintWithSize | Parent vector resize and trusted suffix encoding |
| AppendVarint | Size selection then sized append |
| EncodeFixed32 | Typed fixed32 emission adapter |
| EncodeFixed64 | Typed fixed64 emission adapter |
| DecodeFixed32 | Typed fixed32 accumulation adapter |
| DecodeFixed64 | Typed fixed64 accumulation adapter |
| AppendFixed32 | Typed fixed32 append adapter |
| AppendFixed64 | Typed fixed64 append adapter |
| ConsumeFixed32 | Typed fixed32 admission/diagnostic adapter |
| ConsumeFixed64 | Typed fixed64 admission/diagnostic adapter |
| AppendVarint32 | Typed varint32 append adapter |
| AppendVarint64 | Typed varint64 append adapter |
| EncodeVarint32Trusted | Typed trusted varint emission adapter |
| EncodeVarint32 | Typed output-admission adapter |
| ConsumeVarint32 | Typed checked scalar decoding adapter |
| ConsumeVarint64 | Typed checked scalar decoding adapter |
| AppendLengthPrefixed | Parent vector sizing and trusted prefix/payload encoding |
| VarintLength | Unsigned shift/count sizing primitive |
| EncodeLengthPrefixedTrusted | Trusted prefix emission then payload copy/advance |
| ConsumeVarint32Trusted(ByteView&) | Pointer decoder then span cursor projection |
| ConsumeLengthPrefixedTrusted(const byte*&) | Trusted length decode and pointer slice/advance |
| ConsumeLengthPrefixedTrusted(ByteView&) | Trusted length decode and span slice/advance |
| AsBytes | Borrowed character-to-byte representation projection |
| AsWritableBytes | Borrowed mutable representation projection |
| AsStringView | Borrowed byte-to-character representation projection |

Other block/MemTable definitions are not certified by this bounded batch.
Their existing or future ledgers remain subject to the final every-definition
audit; no short-function or structured-file exemption is introduced.

## Characterization and local commit sequence

Before production edits, add eight public cases in three test commits:

1. Coding: literal varint emission into guarded caller storage, including
   insufficient output and exact cursor retention.
2. Coding: uint32/uint64 truncated, overflow, and excess-terminal matrices,
   with exact messages, unchanged failure cursors, and parent diagnostic provenance.
3. Coding: literal length-prefix failure priority and binary payload borrowing,
   including the non-null borrowed empty value and retained trailing sentinel.
4. Block: literal fast, overlong, and excess-terminal headers, preserving
   successful entry bytes and owned/borrowed backing behavior.
5. Block: partial header and payload admissions, including widened-sum overflow
   adversaries; no visitor callback before entry admission.
6. Block: exact short-footer/count priority and zero-count lazy topology.
7. MemTable: snapshot value/deletion/empty/missing decisions and binary
   lower-bound mismatch, using public lookup results.
8. MemTable: multibyte key/value prefixes and retained arena-backed values
   across subsequent insertions and iterator movement.

Run a fresh Windows Debug build on unchanged production source, including
native lock-child where required. Preserve immutable JUnit/log artifacts and
independently review the new public cases before any production edits.
Then commit scalar/shared-fold, block decoding/layout, and MemTable decisions
as separate local topics, with relevant selected suites after each.
Finally run the integrated selection and independently review the whole diff.
Preserve source annotation multisets; do not edit benchmark paths.

The integrated selection covers CodingTest, BytesTest, BlockTest,
BlockIteratorTest, BlockBuilderTest, MemTableTest, InternalKeyTest,
InternalKeyComparatorTest, LookupKeyTest, WriteBatchTest, VersionEditTest,
TableTest, TableNativeTest, TableBuilderTest, TableCacheTest, LookupTest,
RecoveryTest, and DatabaseTest. Freeze actual original identities/counts from
fresh JUnit rather than assuming existing binary discovery is the baseline.
Intermediate suite identities must equal the relevant original subset.

One published reviewed head must pass every hosted check, including authoritative
GCC 13 branch coverage and portable/Release builds, and actual Windows Debug
logs must match the frozen integrated test names before normal squash delivery.
Ordinary successful cases do not prove allocator failure, physical copy/elision/
destruction counts, dead-read behavior, performance, or whole-source completion.

## Local implementation evidence

The accepted design is commit `d35382e`. Original-source characterization is
split into three reviewable commits before any production edit:
`6759bab` for scalar cursor/diagnostic transactions, `e8f74a7` for block
header/payload/footer admission, and `7166491` for MemTable decisions and
stable packed-record views. Independent bounded review found no significant
case issue.

A fresh unchanged-production Windows Debug build, including the native lock
child, froze 296 unique selected identities in 18 nonempty suites:

| Suite | Cases |
| --- | ---: |
| BytesTest | 5 |
| CodingTest | 26 |
| DatabaseTest | 54 |
| LookupTest | 14 |
| RecoveryTest | 17 |
| TableCacheTest | 7 |
| InternalKeyTest | 9 |
| LookupKeyTest | 6 |
| InternalKeyComparatorTest | 10 |
| WriteBatchTest | 8 |
| MemTableTest | 13 |
| VersionEditTest | 22 |
| BlockTest | 13 |
| BlockBuilderTest | 7 |
| BlockIteratorTest | 26 |
| TableBuilderTest | 19 |
| TableTest | 38 |
| TableNativeTest | 2 |

The production topics remain separate local commits:
`4e37a99` composes scalar byte emission/admission and the three-caller
compatible payload fold; `1731037` composes block header, payload and restart
array admission; `d52f605` composes MemTable user matching and record
classification. Their targeted Windows Debug selections preserve exact
original subsets: 99 identities across eight suites for scalar codecs,
197 across ten suites for block decoding, and 131 across eight suites for
MemTable lookup. The final integrated selection is exactly the original
296 identities, with no failure, error, skip, or duplicate identity.

The complete GCOVR/LCOV/NOLINT annotation multisets are unchanged in coding.cc,
coding_internal.h, block.cc, block.h, and memtable.cc. Documentation validation
and `git diff --check` pass. These are local behavioral/composition gates only;
portable/Release compilers, authoritative branch coverage, hosted Windows
identity parity, and normal merge remain delivery obligations.

Initial integrated review found one portability defect only in the new block
characterization: two bounded size_t values were added directly to vector
iterators, triggering GCC/Clang `-Wsign-conversion -Werror`. The correction was
first committed as `6454ccc` on unchanged production, where all 296 identities
again matched the original set, then normally cherry-picked as `9f8cf08`.
Corrected final Windows Debug results exactly match that corrected original set.

A local GCC Release configuration with the project warning set and
warnings-as-errors compiled all six changed production/test translation-unit
objects. Its attempted full unit target reached an unrelated pre-existing
GCC 16 `maybe-uninitialized` diagnostic in engine/compaction.cc; no unrelated
source was changed to bypass it. GCC 13 hosted coverage and portable/Release
jobs remain authoritative. Independent integrated review reported no other
significant finding.
