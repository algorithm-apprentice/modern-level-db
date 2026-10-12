# Lookup-Key Storage and Shared Internal-Key Encoding

## Status and scope

Proposed recursive follow-up to delivered key comparison/shortening #245.
Source baseline: `7e7b060b2e4bd169f8458cfe9ac052bb13e28c7b`.
Pending metadata publication #248 has independent source/test/design paths;
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
and return it on failure; do not extend it into a boolean compound expression.
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
