# Hash Word and Tail Composition

## Status and scope

Design accepted in #242; the reviewed local implementation is described below.
Recursive follow-up to the
[production composition plan](production-function-composition-plan.md).
Source-read baseline: `80370f2`, after recovered-counter delivery #238.
Block reverse implementation #241 was pending during design and subsequently
delivered on different source/test/design paths; no code dependency required
waiting for it.

Scope is Hash32 and five file-local children in `src/base/hash.cc`,
existing hash characterization tests and this document. No public header,
coding primitive, Bloom/cache caller, allocation policy, coverage or benchmark
change. Other foundation functions remain separate every-definition frontiers.

## Ordered word and tail stages

Hash32 retains its by-value ByteView and sole mutable uint32 hash initialized by
the original seed XOR truncated-size-times-Multiplier expression. Do not widen,
reorder or normalize that arithmetic, copy input bytes, or mutate caller views.
Then call FoldHashWords(input, hash), FoldHashTail(input, hash), and return hash.
All children borrow these parent locals; no second hash owner/result container
or padded temporary byte buffer is introduced.

FoldHashWords retains the original size >= sizeof(uint32_t) loop,
per-iteration const ConsumeFixed32 Result and dereference, then calls
MixHashWord(hash, *word). ConsumeFixed32 remains the existing decoder: do not
replace it with a new raw read or add a redundant failure guard.
Each iteration proves at least four real bytes are available, so its checked
decoder cannot reach the truncated-input error/allocation branch. Preserve
unaligned little-endian decoding and consume-before-mix order.
The Result remains alive through both mix mutations as before.

MixHashWord borrows the same hash and retains the original two mutations:
hash = (hash + word) * Multiplier; hash ^= hash >> 16U.
This coupled unsigned word transform is a leaf. Keep the original uint32
modular arithmetic, multiplier and shift, not a configurable mixer abstraction.

After complete-word consumption, input has zero to three real bytes.
FoldHashTail retains the original nonempty gate and zero-initialized tail.
DecodeHashTail(input, tail) preserves the original ordered byte/shift/OR loop;
then MixHashTail(hash, tail) performs the original multiplication and XOR >> 24U.
Only the tail invokes this distinct final shift. Empty input skips tail
decoding/mixing and preserves seed exactly.
The tail accumulator lives in FoldHashTail across decode and mix. The proven
byte indices 0..2 have shifts 0/8/16; no new guard or unsupported span is needed.

All five helpers retain noexcept like the original enclosing computation.
There are no callbacks, resource transfers, new allocations, captures or
exception conversions. Actual native caller/allocation tests support their
existing observed boundaries; absence of an allocator observer within Hash32
and physical scalar-copy/elision details remain source-inspected.
Do not claim code-generation, call overhead or performance equivalence from
golden outputs; unchanged hosted performance gates still apply.

## Six-definition recursive ledger

| Definition | Responsibility and children/stopping reason |
|---|---|
| `Hash32` | Initialize the seed/length state, compose full-word and residual-tail stages, return the owned scalar. |
| `FoldHashWords` | Consume each complete little-endian word with the existing primitive and mix it in order. |
| `MixHashWord` | Apply one coupled modular full-word multiply/XOR transform. |
| `FoldHashTail` | Select a present residual fragment, decode it and invoke the distinct final mix. |
| `DecodeHashTail` | Assemble zero to three bounded little-endian residual bytes into the initialized scalar. |
| `MixHashTail` | Apply the coupled final-tail multiply/XOR transform with its original shift. |

No wrapper merely renames ConsumeFixed32, byte conversion or a shift.
Separate mixers name genuinely different full-word/final-tail representation
operations; a configurable shift helper would obscure those domains.

## Characterization before extraction

Retain upstream prefix literals for three seeds and lengths 0..12, all existing
unaligned prefixes, empty seed preservation and caller view/storage equality.
Add two public cases on unchanged production:

1. Literal high-seed golden matrices for zero/high-byte inputs through multiple
   complete words and every residual width. Compute/check fixtures independently
   offline; the runtime oracle must be fixed literals, not Hash32 or a copy of
   its loop. Include wrap-prone seeds and empty prefixes.
2. Offset subspans with distinct guard bytes retain pointer/length/storage after
   hashing and match the same literal high-byte vectors. Cover complete words
   and all tail widths at every selected alignment. Guards are real backing
   storage; never forge a huge input or unsupported span to simulate size wrap.

Review literal fixtures and independent derivation before source changes.
Use the anchored coupled native Windows selector:
`^(HashTest|BloomFilterTest|FilterBlockTest|FilterBlockReaderTest|ShardedLruCacheTest|CacheAllocationTest|TableCacheTest|TableTest|TableNativeTest)\.`
Build modern_leveldb_unit_tests and modern_leveldb_cache_allocation_tests.
Require actual nonempty intended classes, unique original identities, both
additions and no failure/error/skip; compare final XML exactly.
Between full-word and tail source topics run `^HashTest\.`.
Measure counts, do not reuse earlier cache/table batch totals.
Preserve all existing fixture/coverage policies and caller behavior.

## Review and delivery

Independent design review and acceptance precede characterization.
Independently review original-source cases before any production edit.
Use two small topics: full-word folding/mixing; bounded tail decoding/mixing.
One non-draft implementation PR may contain both.
Combined independent review checks original arithmetic types/evaluation,
ConsumeFixed32 proof and Result lifetime, tail bounds and local owners, real
Bloom/cache callers, noexcept, literal oracles and observed allocation limits.
Require all exact reviewed-head checks, actual hosted Windows Debug selected
name parity, normal merge and byte-exact reviewed paths in main.
Docs-only checks are acknowledgements, not runtime/performance certification
or completion of the foundation/every-definition production audit.

### Independent design review

The bounded read-only design reviewer traced actual hash/coding contracts,
Bloom/cache callers and existing literal/unaligned hash cases. No significant
issue was found in the six responsibilities, unsigned arithmetic, consume
order, per-iteration Result lifetime, bounded tail shifts, parent-owned locals
or success-path noexcept proof. The reviewer counted 94 source-declared tests
in the nine selected classes, four in HashTest; these are navigation estimates,
not native discovery evidence. New literal fixtures, implementation, selected
XML identities, allocation observations and performance remain unverified.

## Local implementation evidence

Accepted design merge: `fe9f974e21be81008bb277484f50665920b32c6d`.
Original hash production was byte-identical to this merge during
characterization; local original/final coupled runs share that source baseline
for their other files. Independently delivered block reverse #241 must also
pass in the actual hosted PR merge context; do not present the local old-block
run as that integration evidence.

`ce3f84f` adds two public cases before extraction:
`HighSeedsAndAllTailWidthsMatchLiteralBoundaryVectors` and
`GuardedSubspansPreserveCallerViewsAndStorage`.
Four new matrices cover zero/high bytes, seeds ffffffff/80000000 and lengths
0..19: zero through four complete words and all residual widths. Guarded real
27-byte storage covers offsets 0..7 and every prefix, matching fixed literals
and preserving caller pointer/size/whole storage. At the maximum subspan the
end is byte 26, retaining a trailing guard.

The session offline derivation independently checks the actual 80 new C++
literals using little-endian struct-unpack and fallthrough tail addition,
and separately weighted-byte arithmetic with reduce/division. Both also agree
with all 39 retained existing literals. Neither calls production Hash32;
agreement is independent algorithm-port arithmetic, not new upstream evidence.
The bounded characterization reviewer checked the actual literal transcription,
real subspan bounds, existing source, both references and native log/XML.
No significant issue was found.

`c8934f8` extracts the full-word fold and its fixed modular transform.
`96f259c` extracts bounded residual decoding, distinct final mixing and the
nonempty tail selector. All six definitions follow the responsibility ledger.
Original and final coupled Windows Debug runs each contain the same 96 unique
identities with zero failures, errors or skips. Intermediate full-word evidence
contains exactly the six HashTest identities from that set.

| Class | Original/final identities |
|---|---:|
| HashTest | 6 |
| BloomFilterTest | 10 |
| FilterBlockTest | 6 |
| FilterBlockReaderTest | 3 |
| ShardedLruCacheTest | 19 |
| CacheAllocationTest | 5 |
| TableCacheTest | 7 |
| TableTest | 38 |
| TableNativeTest | 2 |

Both unit and cache-allocation targets were built; the five allocation cases
dispatch to their distinct executable. Existing coverage annotation text and
multiplicity are unchanged; no exclusions or thresholds were added.
Guard equality does not prove absence of dead/noncontributory out-of-view
reads. Cache allocation cases do not directly observe Hash32 allocation.
Huge-input physical size wrap, scalar-copy/elision details, code generation,
call overhead and performance remain inspection/hosted-gate limits.
No public header, caller, decoder, benchmark or allocation policy changed.
Hosted reviewed-head checks and actual selected-name parity remain pending.

The bounded integrated code/evidence reviewer inspected the accepted-baseline
source/test/design diff, decoder and real caller contracts, and the supplied
native evidence. No significant issue was found. This read-only review did not
rerun builds/tests and did not remove the explicit hosted integration,
allocation-observer, physical-scalar, huge-input or performance limits above.
