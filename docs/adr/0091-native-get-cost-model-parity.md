# ADR-0091: Native Get Cost-Model Parity

- Status: Accepted

## Decision boundary

Complete the remaining internal pinned-LevelDB Get mechanism alignment before
further isolated optimization experiments. Do not require a separately
selected percentage gain for each mechanism. Preserve observable behavior,
review the composition, verify the actual generated work, and measure the
integrated result honestly.

The reference is authenticated LevelDB
`7ee830d02b623e8ffe0b95d59a74db1e58da04c5`, with the already matched
CRC32C/compression providers and identical Legacy SST image. This is not an
upstream-version update. Errors, exception support, format, defaults, and
delivered optional features remain unchanged. The user permits reviewed public
signature changes; source/ABI changes must be explicit and all overrides,
consumers, tests, and documentation must be updated together.

The user requested a blocker study before alignment. No structural obstacle
was found for the internal changes below. C++23, typed handles, RAII, and
exception support do not require opaque copies, repeated checked decoding,
or additional word-codec calls at every internal layer.

Exact public calling-interface equality is a separate compatibility choice:
Modern's comparator takes two `ByteView` values; Native takes `const Slice&`.
Both views occupy sixteen bytes in the measured MSVC x64 build. Changing a
public virtual signature changes custom overrides and binary compatibility.
Review targeted const-reference boundaries rather than declaring public
signatures immutable or replacing all views indiscriminately.

`ByteView` remains an alias of `std::span<const std::byte>`, not a new
reference implementation. Like `Slice`, it is a nonowning pointer/length
descriptor. Passing that descriptor by value differs from passing a reference
to it; neither operation copies the referenced key bytes.

## What already matches

Direct source inspection confirms:

- Mutable/immutable memtable and Version read pins use plain counters under
  the database mutex, not a shared-pointer increment per Get.
- Get captures its sequence and pins, unlocks during lookup, then relocks
  before charging reads and releasing pins.
- L0 visits overlapping files newest first; higher levels use lazy binary
  lower-bound selection, not eager lookup of all files.
- Seek charging decrements shared file metadata directly, without a
  per-read hash-map lookup.
- LRU uses sixteen shards, complete-key/hash matching, one-allocation entries,
  and intrusive cache/external references with in-use and evictable lists.
- Block search uses the three-short-fields header case, fallback decoding of
  all three varints, binary restart search, and bounded forward replay.
- The matched driver reuses output storage in both engines. It does not
  compare fresh Modern value allocation against retained Native output.

These mechanisms should not be rewritten merely to address disproved
suspicions about `shared_ptr`, `unordered_map`, or output allocation.

## Diagnostic location of the remaining cost

The current owned-process profiles include sixteen captured and sixteen
uncaptured processes, with fixed sixteen-million-Get work per process.
Assign every weighted stack to exactly one nearest source-matched phase.
This puts Native's inlined word decoding and Modern's outlined decoding
inside their real block/filter callers. Preserve unsampled CPU and check
that all phase weights sum to each process's epoch budget.

Modern-minus-Native sampled-epoch mean CPU costs are:

| Comparable phase | One reader, ns/Get | Four readers, ns/Get |
|---|---:|---:|
| Cache operations and pin maintenance | +155.03 | +388.92 |
| Key comparison | +104.49 | +110.11 |
| Index-handle decoding | +86.18 | +75.68 |
| Complete block-iterator phase | -149.90 | -391.60 |

The first three differences are positive in all four paired capture rounds
at each reader count; block-iterator cost is lower in all four.
These are diagnostic captured CPU partitions, not causal savings that can be
recovered by deleting a function. Inlining, phase boundaries, shared-host
variation, unresolved ancestry, and off-CPU waits remain limitations.
Flat `DecodeFixed32` self time alone does not identify the Native gap.

Five analysis contract tests check nearest-caller mapping, comparable
Native/Modern boundaries, unknown frames, and exact exclusive partition.
The original raw profiles, controls, symbols, binaries, and workload remain
unchanged under `build/native-read-attribution`.

## Bounded implementation slices

### 1. Compiler-visible resource pins

Keep current ownership and layouts. Make tiny constructors, moves, empty-pin
destruction checks, and accessors visible to their actual callers for:

- `ShardedLruCacheCore::Pin`;
- `MemTable::ReadPin`;
- `VersionSet::ReadPin`.

The frozen actual Get executable retains several move/destructor callsites
while a cache handle passes through optional/typed wrappers. Its engine Get
also calls out-of-line read-pin construction, moves, destruction, and
accessors. Native borrows existing handles and performs its plain references
at the relevant scope. Static relocation counts are not dynamic call counts,
but they prove that these abstractions are not all eliminated.

Move small definitions into the existing private headers; do not introduce
another handle type, public pin API, reference scheme, allocation, backend,
or type erasure. Keep substantial release/reclamation work out of line.
Include the `VersionSet::PinCurrent` construction wrapper and the cache
pin's null-check/reset envelope, not just an inline destructor that still
calls an opaque `Reset`.
The cache's inline optional-pin wrapper may call an outlined private lookup
that returns the already-referenced entry pointer. Pin wrapping is
nonthrowing; no allocation or callback occurs between acquisition and the
RAII owner. This keeps substantial shard/ref work out of line without
exposing an unowned handle to a caller or changing the typed cache API.
Moved-from handles remain empty and cannot release twice; move assignment
releases its old pin exactly once; self-move remains valid.

Read-pin acquisition/release stays under the database mutex. Existing
exception relocking precedes destruction, and the retired memtable/Version
owners remain live until the final read pin leaves. Cache value destruction
remains outside the shard mutex so deleters can reenter.

### 2. Internal fixed-word visibility

Use the existing internal coding header for shared unsigned, exact-width
fixed32/fixed64 load and store primitives. On little-endian targets,
`memcpy` expresses the complete word operation without alignment or aliasing
assumptions; preserve the byte-assembly/store fallback on every other
endianness. Keep public exported codec declarations and checked-cursor
contracts unchanged.

Use those primitives at proven complete-word internal boundaries: restart
and filter-offset arrays, internal-key trailers, and table/block cache-key
encoding. Address encoding as well as decoding: the actual frozen MSVC
`EncodeFixed64` is also an outlined 202-byte function. Do not infer latency
from its size, or change hash outputs, checksums, persistent bytes, or ISA
requirements.

Permanent characterization covers literal asymmetric/high-bit/all-ones words,
all relevant offsets, exact-size backing, every truncated checked input,
trailing bytes, unchanged cursors/storage, and error source-location parents.
Review all trusted callers' complete-word and lifetime arguments.

### 3. Validated index handles

`Table::Open` already validates each index value as a complete, bounded,
nonoverlapping block handle. The private `IndexHandle` currently feeds it
back through the public `ConsumeBlockHandle` / `ConsumeVarint64` Result chain.

Reuse the existing trusted varint mechanism, extending its unsigned width
without duplicating algorithms, to decode those two complete uint64 values
inside the validated immutable index. Both point Get and table-iterator
loading consume that same boundary. Preserve nonminimal encodings and
unsigned terminal-payload truncation exactly.

Do not change public checked codecs, malformed-index rejection, failure
priority/messages, or backing-file ownership. No unchecked access is valid
merely because a file has an index; `Open` must have established its domain
and the immutable-file contract must keep the validated bytes stable.

### 4. Bytewise sign contract

Modern's public comparator promises negative/zero/positive, not exactly
-1/0/+1. Match Native's use of the `memcmp` sign instead of normalizing it
again. Preserve the zero-length guard for potentially null `ByteView` data,
length tie-breaking, comparator identity, binary keys, and custom comparator
ordering/equality.

### 5. Native-style view-reference boundaries

The proposed comparator signature is
`Compare(const ByteView& left, const ByteView& right) const noexcept`,
including custom overrides and the internal block-comparison forwarding
method. It matches Native's reference-to-view model while retaining the
standard span type, ordering semantics, and Modern's nonthrowing comparator
contract.

An isolated MSVC Release code-generation check uses three identical virtual
forwarding wrappers, all receiving existing view descriptors by const
reference. Forwarding into the actual Modern by-value comparator creates a
42-byte wrapper with two descriptor copies and a call. The proposed span
reference signature and Native's actual `const Slice&` interface each
produce the same seven-byte tail jump. No key/value payload is copied.
This is evidence about this non-inlined boundary, not an end-to-end
throughput result or an argument against every by-value view.

Review equivalent non-inlined Get, lookup-key, and seek boundaries against
their exact Native counterparts. Do not globally replace every `ByteView`:
Native itself passes some internal views by value, inline helpers can
eliminate descriptors, and parser cursors may intentionally be independent
copies. Do not change mutating cursor-reference semantics.

The reviewed boundary ledger is:

| Boundary | Planned reference parameters | Unchanged responsibility |
|---|---|---|
| Public `Comparator::Compare` and all overrides | Both input views | Sign, equality, `noexcept`, and persistent comparator identity |
| Both public `Database::Get` overloads and engine Get | Input key | Output ownership/reuse, snapshots, errors, and exception cleanup |
| Public `Iterator::Seek` and synchronous internal forwarding | Target view | Positioning, corruption recovery, and borrowed-key lifetime |
| Db/Internal/Level/Merging/MemTable/Table/Block iterator Seek chain | Target view and directly forwarded search target | Same native-equivalent search and state transitions |
| `LookupKey::Create` and `CreateTrusted` | User-key view | Existing owned encoding, inline/heap boundary, and moves |
| Internal block comparison forwarding | Both input views | Actual configured comparator invocation |
| Typed/core LRU lookup, hash, match, insertion and erase forwarding | Input cache-key view | Copied key ownership, full-key collision checks, pins, lists, and concurrency |
| `Hash32` and Legacy Bloom/filter query forwarding | Input key/filter descriptors | Identical hash outputs, serialized filter bits, probe count, and malformed-filter policy |

The LRU row includes `HandleTable::Lookup/FindPointer/PrepareInsert/Remove`,
`Entry::Matches`, and the corresponding core helpers. CLOCK keeps its
existing fixed-extent key representation; the opaque facade only borrows the
input descriptor during its synchronous call.

Returned/stored descriptors, independent parser cursors, generic SkipList
borrowing, unrelated write APIs, and pointer-sized fixed-extent spans are
outside this signature migration. Public custom-comparator binaries must be
rebuilt; old overrides must not be mixed with the new interface.

Temporary view arguments remain live for the complete call, but a reference
to a descriptor never extends the lifetime of its backing storage. No
comparator or synchronous read may retain a reference to a caller's view.
Document required custom-override migration; do not introduce a custom Slice
class, borrowed-reference owner, or duplicate public comparator hierarchy.
Do not replace custom comparators with bytewise comparison or confuse a
parameter-passing change with permission to change persisted order.

## Retained contracts and genuine decisions

Do not copy Native behavior that would remove minimum internal-key checks,
permit length-sum wraparound, form out-of-bounds restart pointers, or bypass
Modern's allocation/callback exception cleanup.

Public Error/Result behavior does not imply successful Get allocates an
Error. The measured 72-byte Modern Status versus eight-byte Native Status
also does not prove Native returns it in a register: its nontrivial
destructor matters to the ABI. Judge actual calls and success-path work.

LookupKey's initialized inline backing and canonical moved-from contract
remain unchanged. Native's noncopyable/nonmovable owner cannot simply
replace it. Earlier partial-initialization/move regressions are evidence,
not a fundamental prohibition on a future reviewed ownership design.

Strict filter/index metadata rejection and Native's permissive handling have
Open-time differences. Broadening acceptance or changing corruption recovery
needs an explicit contract decision, not a hot-path shortcut.

Full filters, CLOCK, default-off reader hints, complete-key borrowing, and
bounded reverse replay retain their current behavior. Their presence does
not prevent native-aligned shared mechanisms in the corresponding Legacy/LRU
configuration. No option/default or earlier delivered feature is removed.

## Delivery and acceptance

Review this design independently before production edits. Use small,
test-first commits for each slice, with source-level caller/lifetime proofs
and actual archive-to-linked-function checks. Exercise pin moves, old-value
release, moved-from destruction, self-move, replacement/erase, exception
relocking, and retired-owner lifetime.

Measure the integrated composition against the frozen baseline, with fixed
work, balanced serial processes, unchanged Native controls, and relevant
Legacy/full-filter and constrained/resident-cache cells. Retain negative
controls and adverse results. Do not turn per-slice performance into another
optimization search or claim that all differences have disappeared.

Independent implementation/evidence review, ordinary exact-head quality
gates, normal merge, and merged-branch cleanup remain required. Every public
ABI change and any further retained mechanism must remain explicit, not
silent scope expansion.
