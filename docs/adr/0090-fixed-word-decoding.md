# ADR-0090: Fixed-Word Decoding

- Status: Measurement-only

## Outcome and current scope

Do not adopt an isolated restart-read change from this experiment. The
longer-window replication retained and independently verified all 60
processes, including the exact-executable Modern A/A controls.

| Treatment | One-reader paired wall change | Faster pairs | Four-reader paired wall change | Faster pairs |
|---|---:|---:|---:|---:|
| Word | -1.41% | 5/6 | -0.76% | 4/6 |
| Inline byte | +4.80% | 0/6 | +7.29% | 1/6 |
| Both | -3.44% | 6/6 | -0.44% | 3/6 |

The A/A median absolute differences were 2.11% and 3.42%, respectively;
individual same-executable pairs reached +7.84% and +11.48%. The additional
A+B-versus-A contrast was -2.82% in five of six one-reader pairs but +0.39%
in only three of six faster four-reader pairs. A general multi-reader Get
benefit or necessary combination is not established.

The independently reviewed portable primitive design below remains useful
context. Its isolated adoption plan is replaced by the complete Native
cost-model alignment in [ADR-0091](0091-native-get-cost-model-parity.md).
That work addresses cache/read-pin propagation, encoding as well as decoding,
and private handle decoding as a composition, not by selecting favorable
single-factor measurements. No production implementation was changed by
the 48-, 96-, or 60-process campaigns.

## Concept and observed mismatch

The persistent format requires little-endian integers, not a particular
instruction sequence. A decoder can preserve that representation without
assembling each byte separately on a little-endian host.

The current public fixed32/fixed64 decoders accept static-extent spans and
remain out of line. In the measured MSVC Release objects, their byte-assembly
template produces 31- and 206-byte functions. The diagnostic word-load
replacement produces a three-byte fixed32 function and a four-byte fixed64
function, without changing the public signatures.

Owned-process CPU profiles put substantial Modern self time in
`DecodeFixed32`; equivalent Native work is often inlined into its callers.
Those percentages are not the fraction of the Native performance gap
attributable to decoding. A compiled intervention and an unchanged control
are required before making a throughput claim.

The locally reviewed RocksDB revision
`553b52b4a709820fac1dbadc7796506740517582` uses inline, alignment-safe word
copying in `util/coding_lean.h`. This is a mechanism reference, not a proposal
to import its block formats, iterator ownership, or other read-path changes.

## Bounded design

Factor A changes only fixed-width decoding. For a little-endian host, copy
exactly `sizeof(UInt)` bytes into an unsigned result with `std::memcpy`.
For every other native endianness, retain the existing unsigned byte-assembly
loop. Select the implementation with `if constexpr`, not runtime dispatch.
Do not use a typed unaligned pointer dereference.

Keep the existing public `DecodeFixed32` and `DecodeFixed64` declarations,
symbols, static span extents, and `noexcept` contracts. Checked
`ConsumeFixed32` and `ConsumeFixed64` retain their length checks, exact
consumption, error messages, source-location parent, and unchanged cursor on
failure. Encoding, varints, checksums, hashes, and all persistent bytes are
unchanged.

Factor B gives the hot block restart-word read visibility of the same
primitive. If the complete evidence supports adopting B with A, put one
internal inline `DecodeFixedTrusted` template in the existing
`base/coding_internal.h`. Public out-of-line decoders and their checked
consumers delegate to it; `Block::Layout::RestartPoint` invokes it directly.
Do not duplicate the endian algorithm in block code or publish a new public
codec. If B is not adopted, keep A's helper private to `coding.cc` instead.

The shared helper statically requires an unsigned result and a span extent
equal to its width. It neither validates dynamic input length nor advances
a cursor. Callers must provide a complete word in live, stable backing
storage. The change introduces no allocation, mutable shared state, new ISA
requirement, or runtime option.

Do not combine this change with partial-varint block-header decoding,
inline iterator-key storage, comparator dispatch, LookupKey construction,
cache-policy changes, hash refactoring, or a file-format change.

## Bounds and lifetime argument

Public static-extent callers already promise a complete word.
`ConsumeFixed` establishes the same domain with its existing size check
before constructing the fixed span.

`Block::Create` checks that the trailing restart count and complete restart
array fit in the block, then derives `entries_end` and `restart_count`.
Every `Layout::RestartPoint(index)` caller establishes
`index < restart_count` through a positioning guard, bounded loop, or the
existing valid-position precondition for relative traversal; its four-byte
span is therefore inside that array.
Non-iterator callers are `ValidateRestartEntry`'s guarded next index,
`empty`'s nonzero-count first word, and `ValidateEntries`' guarded first word.
Iterator positioning excludes a zero restart count before selecting zero or
the last index. `FindRestart` keeps its search endpoints inside that range;
`AdvanceRestartIndex` bounds each next index. Relative reverse traversal
starts from a valid position and only decrements its restart index.
Restoring a reverse-cache entry restores the corresponding bounded index
saved from a previously valid entry, not a new index decoded from input.
The immutable `BlockContents` owner or mapped-file lifetime keeps these bytes
live for the iterator. The existing iterator checks still reject decoded
restart offsets outside the entry region and malformed restart entries.
An in-bounds array word is not proof that the offset it encodes is valid.

The helper must not weaken those checks, add speculative wider reads, rely on
alignment, or reinterpret bytes as an integer object. Document its proven
preconditions rather than adding repeated runtime assertions.

## Completed diagnostic evidence

The first out-of-line word experiment retained 48 fresh processes. Modern
improved in several cells, but relinking also moved unchanged Native code.
Its apparent Native-gap reduction is not accepted as decoder attribution.

A separate four-cell campaign replaced the coding and block archive members
in every Modern arm. All Native timings used one immutable reference
executable. All 96 serial processes passed independent workload, binary,
affinity, checksum, topology, freshness, and cleanup verification.

The factors were general fixed decoding (A) and restart-reader inline
visibility (B). Costs are paired median changes in fixed-work wall time;
negative means faster, and four-reader cost is reciprocal aggregate
throughput, not request latency.

| Modern treatment | One reader | Faster pairs | Four readers | Faster pairs |
|---|---:|---:|---:|---:|
| A: word loads, existing restart call | -3.60% | 4/6 | -0.86% | 4/6 |
| B: inline existing byte assembly | +5.30% | 1/6 | +6.30% | 0/6 |
| A+B: inline word restart read | -2.86% | 5/6 | -2.80% | 6/6 |

The four-reader A+B CPU-cost change was -2.34%, with five of six faster
pairs. Absolute wall-time interaction
`T(A+B) - T(A) - T(B) + T(base)` was negative in six of six four-reader
rounds, with a median of -123.345 ns per aggregate operation.

These results are promising, not an admission decision. The identical Native
executable still showed substantial slot-to-slot variation, including one
single-reader pair at +17.68%; its four-reader false interaction was negative
in five of six rounds. Do not subtract that drift, pool the campaigns,
discard unfavorable rounds, or claim a uniquely identified microarchitectural
cause. Actual code generation also changes outlining decisions: the
byte-inline-only `FindRestart` calls an outlined `RestartKey`.

All four block variants match 49,152 Native seek cases across twelve
key-width, value-width, and restart-interval shapes. Both primitive variants
match 1,048,576 unaligned fixed-word cases against independent byte and Native
oracles. These are diagnostic executable checks, not replacements for
permanent unit and compatibility coverage.

## Predeclared longer-window replication

Before adopting an implementation, retain a separate 60-process Modern-only
replication using the exact four frozen executables, with no recompilation.
Each process performs eight million fixed Gets. Six balanced rounds cover
one and four readers and five labels: base A, base B, word, inline, and both.
The two baseline labels invoke the exact same executable path and hash,
not separately linked no-op copies.

Alternate the reader-block order and balance cell positions with reversals.
Keep the same Legacy image, API, checksums, providers, logical data-plus-filter
budget, affinity, verification, and untimed warmup. Run one owned workload
process at a time without stack sampling or concurrent local builds.

Use the predeclared arithmetic mean of the two baseline costs within each
round as the anchor. Also report both individual baseline contrasts, every
A/A difference, CPU cost, and absolute interaction. Retain every case.
Seven executable contract tests cover scheduling, identity, work, invalid
metrics, raw/saved disagreement, and reporting math. An excluded preflight
verifies the eight-million-operation checksum.

This follow-up estimates Modern treatment effects and the method's noise
floor; it is not a new Native-gap estimate. Shared-host activity, frequency
variation, and Modern compiled layout remain limitations.

Independent design review also requires an explicit incremental contrast:
report per-round `T(A+B) - T(A)` and `T(A+B) / T(A)` for wall and CPU cost at
both reader counts. This reporting amendment was added after freezing the
replication execution and before inspecting its timing results; it does not
change the execution or its declared baseline anchor. Mark the same contrast
on the completed 96-process campaign as additional analysis.

Negative interaction alone cannot justify adopting B: removing B-alone's
regression can produce a negative interaction without outperforming A.
Choose the simpler A-only implementation unless the combined mechanism has
adequate incremental justification. Do not relabel recovery from a harmful
single factor as proof of extra combined throughput.

## Admission and delivery

No preset percentage speedup is a gate. Require preserved behavior, bounded
implementation complexity, verified actual code generation, and honest
local/whole-Get evidence. If end-to-end gains cannot be distinguished from
the controls, do not claim a demonstrated Get improvement. An unexplained
reliable global regression blocks adoption. The adverse inline-byte-only
result must remain visible even if the combination is selected.

Review this design independently before changing production code. First add
permanent fixed32 and fixed64 characterization using literal high-bit and
all-ones vectors at every offset zero through seven, exact-width backing,
every truncated length, exact-input success, and success with trailing
bytes. Check the cursor pointer and size, unchanged input bytes, exact
corruption messages, and the `coding.cc` / `ConsumeFixed` source-location
parent. Sentinels demonstrate unchanged storage, not absence of overreads;
exact-size allocations under the ordinary sanitizers cover that boundary.

Then implement A and, only if justified, B in small reviewable increments.
If B is adopted, document the existing `Next`/`Prev` valid-position
precondition next to the internal block-iterator declarations, without
runtime assertions or altered invalid-position behavior. Existing
malformed-block, ordering, lifetime, and Native-compatibility coverage must
remain green.

Verify the integrated optimized functions and callers rather than assuming
the diagnostic prototype's layout survives the shared-helper implementation.
Measure relevant default full-filter and constrained-cache controls before
claiming a general read benefit. Do not claim big-/mixed-endian execution or
cross-platform speedups from Windows x64 results.

Independent implementation/evidence review and ordinary exact-head quality
gates precede pushing and normal merge. The delivered decision and its
source/test index must identify which factors were actually adopted.
