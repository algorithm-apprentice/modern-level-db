# ADR-0091: Native Get Cost-Model Parity

- Status: Implemented

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

Before this alignment, Modern's comparator took two `ByteView` values while
Native took `const Slice&`.
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

The pre-alignment owned-process profiles include sixteen captured and sixteen
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

The implemented comparator signature is
`Compare(const ByteView& left, const ByteView& right) const noexcept`,
including custom overrides and the internal block-comparison forwarding
method. It matches Native's reference-to-view model while retaining the
standard span type, ordering semantics, and Modern's nonthrowing comparator
contract.

An isolated MSVC Release code-generation check uses three identical virtual
forwarding wrappers, all receiving existing view descriptors by const
reference. Forwarding into the original Modern by-value comparator creates a
42-byte wrapper with two descriptor copies and a call. The implemented span
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

| Boundary | Implemented reference parameters | Unchanged responsibility |
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

## Integrated outcome: October 23, 2026

The five slices are implemented at
`2570b9f0628fcf9cb1366aa9433629ffc6803a8f`, following separate independent
architecture/ABI and implementation reviews. The implementation review found
no significant issues. A third independent evidence review recomputed the
complete campaign before publication.

The measured source trees are:

- `src`: `9869858df64f217b7501abe7fe561ca607cc7ffb`;
- `include`: `a426540434bb14f248c255ef8549e83ed59ee9f5`.

The integrated experiment rebuilds all sixty Modern core translation units
with the original normalized compiler commands; only object/PDB output
locations differ. Every archive member matches its recorded object. The
same driver source and hook source are recompiled against the candidate's
matching changed header ABI rather than mixing an old caller with new
symbols. Baseline Modern is `9320351`; Native is the pinned revision above.
All Native controls and baseline Modern use one immutable original
executable. The unused Native copy inside the candidate is never timed.
CRC32C, Snappy, and Zstd providers remain frozen and identical.

Actual linked caller bodies match their archive objects and have no opaque
tiny pin construction/move/destruction/accessor calls or hot exported
word/checked-handle codec calls. The public reference-forwarder proof uses
the actual migrated interface, not only a hypothetical test class.
Function sizes and removed references establish generated-work changes,
not individual elapsed-time savings.

### Fixed-work results

All 108 fresh workload processes ran serially, one owned workload at a time.
Each executed eight million Gets over the same million-record, 256-byte-value
corpus. Six balanced rounds rotate cell order and reverse role order. The
Legacy cells use original Modern, aligned Modern, and two labels invoking
the exact same immutable Native program. Full-filter cells compare Modern
before/after only. Fifteen independent functional preflights are excluded.

Both libraries use reusable owning output buffers, checksums, the same
providers, and matched decoded-data-plus-selected-filter budgets. Verification
and point warmup are outside timing. Every raw result passes workload,
binary/command/environment, affinity, topology, ownership, clone-isolation,
and cleanup checks. Monotonic invocation intervals independently establish
that no workload processes overlap; total accepted work is 864 million Gets.

Changes below are median within-round after/before cost ratios. Negative
means less time. Four-reader wall cost is reciprocal aggregate throughput,
including the matched launch/join overhead, not request latency.

| Image / logical cache / readers | Wall cost change | CPU cost change | Faster pairs, wall / CPU |
|---|---:|---:|---:|
| Legacy / 512 MiB / 1 | -11.88% | -11.71% | 6/6 / 6/6 |
| Legacy / 512 MiB / 4 | -7.15% | -8.02% | 6/6 / 6/6 |
| Full / 512 MiB / 1 | -8.70% | -8.87% | 6/6 / 6/6 |
| Full / 512 MiB / 4 | -9.08% | -9.13% | 6/6 / 6/6 |
| Legacy / 8 MiB / 4 | -6.31% | -6.11% | 6/6 / 6/6 |
| Full / 8 MiB / 4 | -4.17% | -4.94% | 5/6 / 5/6 |

The Full/8-MiB/four-reader third round is adverse: wall cost increases
1.217269% and CPU cost increases 0.745629%. It remains in every summary;
the campaign is not described as winning every pair.

### Remaining Native difference and control variation

The predeclared Native comparison anchor is the arithmetic mean of the two
identical Native slots within each round. It is a same-cell cost reference,
not subtraction of control drift from the Modern treatment effect.

| Legacy cell | Original Modern extra wall cost versus Native | Aligned Modern observed difference versus Native | Native A/A median absolute wall variation |
|---|---:|---:|---:|
| 512 MiB / 1 reader | +11.93% | -2.00% | 2.64% |
| 512 MiB / 4 readers | +6.46% | -1.27% | 2.84% |
| 8 MiB / 4 readers | -0.38% | -5.60% | 1.77% |

The resident results support approximate parity in this campaign, not a
formal equivalence result or a general claim of beating Native. The A/A
figures are descriptive variability measures, not confidence bounds.
For the single-reader resident control, `native_b` is slower in all six
pairs, by 0.451544% through 6.570261%, despite identical code. This unexplained
directionality is retained explicitly; the control is not noise-free.

The integrated treatment consistently improves its own before version, but
the experiment cannot allocate the improvement to individual pin, view,
word, handle, or comparison changes. Driver/header ABI and compiled-layout
effects are part of the composition. Do not add the earlier profile phase
deltas, subtract Native drift, or pool this campaign with the rejected/noisy
isolated experiments. Default Full results above are versus Modern's own
before, not an unfiltered Native baseline.

Scope remains this shared Windows x64 host/compiler, warmed fixed SST corpus,
and logical cache budgets, not RSS. No tail-latency, cold-device, mixed/write,
cross-platform speedup, or single-reader pressure result is claimed.

### Verification and retained evidence

The exact production library additionally matches 1,048,576 unaligned word
decode cases, 2,097,152 guarded word-store cases, and 49,152 Native block seeks.
Permanent tests cover exact-size outputs/inputs, cursor/error contracts,
nonminimal uint64 varints and excess terminal payloads, pin moves/reclamation,
and exhaustive unsigned comparison signs. Local validation passes 1,110
unit/public cases, all nineteen Native/extended cases, and thirteen CMake
consumer checks. All twenty ordinary checks pass at the reviewed source head.

Independent evidence review verifies all 123 raw process records, all sixty
archive members, original compiler flags, source/header identity, actual
linked instructions/relocations, the complete paired math, and every A/A
and adverse observation. No workload is rerun or selected away for review.

Retained artifacts are under
`build/native-read-attribution/native-parity-integrated`, including
`inputs.json`, `build-inputs.json`, `compile-commands.json`, `linked-proof.json`,
`production-oracles.json`, the complete raw process directories, and
`verification.json`. Build seal:
`5edf1e4826ca1ae7ddf6c5910c03720737e39e54abb1479afe0025f5780107b4`.
Campaign seal:
`1a4de7e29c3794d74eeb5b3cbeed82343d63ee4d57d460c880a59986e36aea25`.
These identities distinguish this integrated campaign from ADR-0090's
measurement-only factorials.
