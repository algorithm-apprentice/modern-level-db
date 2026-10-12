# Lookup-Key Storage and Shared Internal-Key Encoding

## Status and scope

Implemented recursive follow-up to delivered key comparison/shortening #245.
Source baseline: `7e7b060b2e4bd169f8458cfe9ac052bb13e28c7b`.
Design accepted through #249. Metadata publication #248 has independent
source/test/design paths and was delivered while original cases were developed;
it is not a semantic prerequisite. No benchmark work is included.

Three small production topics may share one non-draft implementation PR:
trusted byte emission shared by two actual callers and lookup storage setup;
move-assignment/canonical-reset composition; internal-key input admission.
Scope: src/format/internal_key.cc/.h, directly dependent
src/memory/memtable.cc, existing tests/unit/format/internal_key_test.cc and
this document. Existing MemTable/caller cases stay in the coupled selection.
No public include API, wire format, allocation strategy, sequence/kind policy,
storage threshold, error wording/priority, runtime assertion, conditional
diagnostic compilation or coverage policy changes.

## Shared trusted representation emission

Direct source reading finds identical user-byte copy, cursor advance and
fixed64 packed-trailer stores in LookupKey::CreateTrusted and the private
EncodeMemTableEntry. Keep each caller's length-prefix and allocation policy
separate. Add EncodeInternalKeyTrusted to the existing internal format header
and define it in internal_key.cc, not a new public utility/header.

It borrows MutableByteView by reference, user key, sequence and kind; requires
enough output and already valid 56-bit sequence/kind, emits the existing user
bytes followed by fixed64 trailer, then advances past the complete record.
Use existing ranges::copy, PackInternalKeyTrailer and EncodeFixed64; no owner,
temporary vector, generic callback or checked error path.
The helper's noexcept follows those trusted non-allocating primitives.

MemTable keeps its parent arena allocation and saved entry pointer, then
varint prefix, shared internal-key emission, existing value emission and
return. Its two original cursor advances remain inside the shared helper.
Lookup keeps parent wire-layout constants, canonical result owner and output
span, then length-prefix emission and the same shared body with SeekValueKind.
It additionally advances its otherwise-unused local cursor past the trailer.
This is an intentional local-cursor change, not a claim of identical physical
instruction/span-check counts; no public view or subsequent operation observes
that cursor. Output allocation and written bytes remain unchanged.

Do not use this helper to rewrite EncodeUnchecked's reserved vector through
resize/zero-fill; its allocation/materialization contract differs. The existing
vector reserve/insert/AppendFixed64 operations remain its named children.
Do not unify length-prefix policy with MemTable's value layout or add guards
for practically unrepresentable lengths; existing uint32 casts and trusted
length preconditions are preserved.

## Parent-owned lookup storage preparation

CreateTrusted retains the original const internal-key size, uint32 converted
size, prefix size and encoded size in its parent before constructing result.
The representation-linked scalar calculations do not need a layout parameter
bag. PrepareStorage(encoded_size, prefix_size) borrows the result through this:
allocate heap only when encoded_size > InlineCapacity (200), then assign
encoded_size_ and internal_key_offset_ in their original order.
Its sole caller is a freshly canonical result, not a storage-resizing API.
No new heap reset, preliminary resize, failure catch or noexcept is added.

Result construction still runs ResetToCanonicalEmpty before heap allocation.
The parent output span is constructed only after storage preparation and stays
alive through byte emission. Checked Create keeps its one sequence boundary
and delegates to trusted CreateTrusted as before.

## Move assignment and canonical state

Move assignment keeps the original this != &source gate and *this return.
TransferStorageFrom borrows source and preserves complete inline-array copy,
heap-owner move, encoded size and prefix offset assignment in that order.
The parent then calls source.ResetToCanonicalEmpty once.
Its noexcept follows the original representation-transfer operations.
Self move neither transfers nor resets.

Do not route the move constructor through assignment/default construction:
keep its original member initializers and source reset, without extra
canonical writes or changing member construction order. Its coupled member
initialization is one complete representation transfer, and std::move/reset
are already named children.

ResetToCanonicalEmpty keeps heap_storage_.reset() first, then calls
InitializeCanonicalEmpty. That child installs the original nine-byte,
one-byte-prefix representation: encoded size 9, internal offset 1, prefix 8,
fixed64 PackInternalKeyTrailer(0, SeekValueKind). It does not clear the unused
inline tail. Complete layout/prefix/trailer publication is one coupled
canonical-state operation, not separate independently callable field setters.
Default construction keeps its existing named reset child.

Borrowed views must be reacquired after moves, including inline relocation.
Tests may compare saved pointer values but must not dereference invalid
pre-move views; heap pointer transfer and canonical source behavior are public
observations, not general allocator-failure certification.

## Internal-key input admission

InternalKey::Create keeps encoding/ownership in the parent and calls
ValidateInternalKeyInput(sequence, kind) first. Retain a mutable local Status
and move its error into the parent's Result on failure; do not extend it into
a boolean compound expression.
The file-local child performs the original sequence > MaxSequenceNumber
boundary first, then unsupported kind, preserving both complete error literals.
Do not merge with checked LookupKey::Create's differently worded error through
an allocation-affecting label parameter. No new accepted sequence/kind.
EncodeUnchecked remains the vector-owning materialization primitive, not a new
buffer-returning staging owner. Additional Status transport is not a physical
copy/elision, allocation-count or timing guarantee.

## Ten-definition ledger

| Definition | Children or responsibility-based stopping reason |
|---|---|
| `LookupKey::CreateTrusted` | Parent layout/owner compose storage, varint prefix and shared body. |
| `LookupKey::PrepareStorage` | Select and install the one freshly created seek target's backing representation. |
| `EncodeInternalKeyTrusted` | Borrowed user-byte copy and fixed packed trailer compose complete trusted record emission/cursor advance. |
| `EncodeMemTableEntry` | Existing arena/entry owner, length prefix, shared record, existing value primitive. |
| `LookupKey::operator=` | Self guard, representation transfer, canonical source reset. |
| `LookupKey::TransferStorageFrom` | Coupled inline/heap/size/offset ownership transfer. |
| `LookupKey::ResetToCanonicalEmpty` | Release heap, then install complete canonical inline state. |
| `LookupKey::InitializeCanonicalEmpty` | Coupled nine-byte seek representation with original layout and bytes. |
| `InternalKey::Create` | Ordered input admission, existing owned encoding. |
| `ValidateInternalKeyInput` | One ordered sequence/kind construction admission contract. |

Unchanged move/default constructors are inspected as described above; the
ledger does not certify all internal_key.cc/.h or MemTable functions.
Parser/borrowed-view construction, Decode/EncodeUnchecked, accessors and
inline trailer operations still participate in the final every-definition
audit; prior comparison/shortening acceptance is not a whole-file exemption.

## Original-source characterization and delivery

Retain existing golden encodings, 190/191 inline/heap move boundary, binary
keys, canonical moved-from lookup, invalid input, MemTable packed entry,
snapshot/tombstone, table lookup and engine read cases.
Add three public cases before production edits:

1. Complete seek-byte matrix over user lengths 0/118/119/120/190/191/512 and
   sequences zero/max. Explicit literal varint prefixes cover the 127/128
   transition and encoded total 200/201; literal trailers cover zero/max.
   Patterned user bytes include binary values. Compare full memtable/internal/
   user views, guarded caller storage and checked/trusted results without
   producing expected bytes through production varint/trailer encoders.
2. Inline/heap source/destination move matrix at lengths 0/190/191/512:
   construction, all assignment combinations, self move, exact canonical
   source bytes and reacquired destination views. Save owned byte expectations
   before moving; no stale borrowed-view reads. Test original heap pointer
   transfer vs inline relocation without exposing private capacity flags.
3. Internal construction sequence/kind error-priority matrix, including both
   invalid together, each alone and valid boundary cases. Compare exact errors
   and retain existing literal valid trailer expectations.

Build unit/Windows-lock-child executables and freshly measure the anchored
coupled selection:
`^(InternalKeyTest|InternalKeyComparatorTest|LookupKeyTest|MemTableTest|TableTest|TableNativeTest|TableCacheTest|LookupTest|DatabaseTest)\.`
Check actual class presence and case identities, not source totals or older
other-module XML. Intermediate selector:
`^(InternalKeyTest|InternalKeyComparatorTest|LookupKeyTest|MemTableTest)\.`
Require original/final exact identities with zero failure/error/skip.

Independent source-backed design review and accepted non-draft design PR
precede characterization; independent original-case review precedes production
topics; integrated code/evidence review precedes non-draft implementation PR.
Preserve coverage/lint annotation full-text multisets. All exact reviewed-head
checks and actual hosted Windows Debug selected identities must pass before
normal merge, own-path equality, main fast-forward and branch cleanup.
No allocation-failure, benchmark, hypothetical copy or every-definition
completion certification is inferred from ordinary successful runs.

### Independent design review

The bounded read-only reviewer independently traced the proposed responsibilities,
actual shared byte-emission callers and supporting contracts/test oracles.
No significant issue was found. No edits, execution or nested agents were
performed. Proposed implementation, allocation behavior, physical instruction
counts, hosted identities and every-definition completion remain unverified.

## Original-source cases and implemented topics

Three public cases and the test-only complete-view observer were committed as
`851b868` before production edits. All three scoped production paths were
byte-equal to accepted design baseline `e29bebc549e5ea6fb6394b81ce94a7c46973fb67`.
The original freshly built native selection measured 151 unique identities
across all nine requested classes, with zero failures/errors/skips.
The bounded independent original-case reviewer traced original source,
contracts, literal/layout/move/error oracles and supplied XML/logs, and found
no significant issue. No edits, builds, execution or nested agents were used.

The literal seek matrix has 14 rows at the seven planned lengths and two
sequence boundaries. Complete expectations use explicit varint/trailer
literals plus guarded patterned user bytes, not production encoding calls.
The four-size move case covers constructor transfer, all 16 assignment
combinations and self assignment, exact canonical source bytes, pointer
transfer/relocation and reacquired complete views. Its owned pre-move bytes
are a behavioral transfer oracle, not an independent wire encoder; the separate
literal case supplies that evidence. Construction admission checks four invalid
combinations with exact sequence-before-kind priority and four literal valid
boundary encodings. Saved pointer values are compared, never dereferenced after
their borrowed views become invalid.

After #248 passed all exact-head checks and actual hosted original-name proof,
its accepted main `8cb8bceaef0342fdac2b68bc1ee5d6c17c765466` was integrated by
normal merge `9d26438`; original case commit/history was not rewritten.
All three scoped production paths still matched the original key baseline.
A fresh integrated-original build measured exactly the same 151 identities
before any key production edits, while preserving the earlier XML/logs.

Three small production topics follow that evidence:

1. `9d1a0b5`: shared internal-key emitter with two actual callers; lookup
   backing storage preparation. Parent allocation/layout/prefix/owner and
   MemTable's saved entry/value cursor remain visible.
2. `3cf8559`: assignment's complete four-member transfer before source reset,
   self guard and original move-constructor initializers; reset releases heap
   before the coupled canonical inline layout/prefix/trailer child.
3. `d959b96`: ordered sequence/kind construction admission. Parent mutable
   Status remains live through encoding; its failed error is moved into the
   Result<InternalKey>, since the two return types are distinct.

The internal header supplies the trusted emitter contract and private member
declarations. Only now-unused MemTable algorithm/span includes were removed.
The documented final unused LookupKey cursor advance is intentional; no
public representation changes. EncodeUnchecked, parser policy, public checked
lookup admission and comparison/shortening logic remain unchanged.
No generic layout bag, extra encoding owner, vector resize/zero-fill strategy,
new guard/assertion, error label parameter or annotation was introduced.

### Measured local native identities

Original, integrated-original and final nine-class native sets match exactly:
InternalKeyTest 9, InternalKeyComparatorTest 10, LookupKeyTest 6,
MemTableTest 11, TableTest 38, TableNativeTest 2, TableCacheTest 7,
LookupTest 14 and DatabaseTest 54; total 151 unique identities.
There are zero failures/errors/skips, and all three added names are present.
After each of the first two production topics the four-class intermediate is
exactly the original 36-identity subset, not merely an equal count.
All three production source/header annotation full-text multisets match
accepted main, including empty sets, without new exclusions.

Persistent evidence: `lookup-storage-before.xml/.log`,
`lookup-storage-integrated-before.xml/.log`, `lookup-storage-after.xml/.log`;
intermediates: `lookup-storage-encoding.xml/.log` and
`lookup-storage-move.xml/.log`. Older module totals are not substituted.
Ordinary successful native execution is not allocation-failure, hypothetical
copy/elision, benchmark or every-definition certification.
Integrated review, authoritative GCC 13 diff-branch coverage, all exact-head
CI checks and actual hosted selected-name parity remain delivery gates.

### Independent integrated review

The bounded read-only reviewer independently traced the five scoped paths
against accepted main through all three production topics and evidence.
No significant issue was found. No edits, builds, execution or nested agents
were performed. Source and supplied identity evidence do not certify the whole
file, allocator-failure behavior or performance. Hosted CI, actual hosted
selected-case parity and authoritative GCC 13 coverage remain pending.

### Release fixture-construction correction

First implementation head `5a35e73` passed the GCC 13 coverage job, but portable
checksum and Linux Release builds rejected the new test's guarded vector
front/back writes with `-Werror=null-dereference`. The actual seven fixture
lengths are bounded literals; production encoding was not implicated.
Replace size-plus-two construction and front/back writes with an explicit
leading-byte initializer, identical patterned pushes and final trailing-byte
push. The complete input/guard bytes, offsets and 14 rows remain unchanged,
and views are still formed only after all pushes.

The correction was first executed on unchanged key production in original
branch commit `c54a0b3`, with all six LookupKeyTest identities passing.
An independent limited original-case review found no significant issue.
Normal cherry-pick `33fcbc1` ports it to the published implementation without
rewriting history, changing production or weakening compiler/coverage flags.
Corrected final `lookup-guard-after.xml/.log` has exactly the same original
151 identities; `lookup-guard-before.xml/.log` is exactly its six-name lookup
subset. Original and first-final evidence/logs are preserved.
Authoritative new-head Release/portable compilation and all delivery gates
remain pending; successful local execution does not substitute for them.
The independent final changed-scope reviewer traced the fixture/doc correction
and retained evidence and found no significant issue, without edits or execution.
