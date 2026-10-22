# ADR-0083: Direct LookupKey Construction

- Status: Implemented

## Decision

Construct the final LookupKey representation directly. Preserve fully
initialized inline storage and the original fixed-size move implementation.
Accept the repeated local construction/drop benefit without claiming a
whole-Get speedup or a move speedup.

The factory already knows the encoded size and prefix offset. Its private
constructor now selects backing and initializes those metadata fields once.
It does not first construct a canonical empty key that the factory immediately
overwrites. Heap backing uses `make_unique_for_overwrite`: encoding writes
every active heap byte before returning a completed object.

The 200-byte inline array remains value-initialized. Move construction,
assignment, canonical moved-from state, self-move, view lifetimes, and class
layout are unchanged. This deliberately rejects a more aggressive
partial-initialization/prefix-copy prototype with repeatable inline-move
regressions.

## Correctness boundary

The same trusted key-length and published-sequence preconditions apply.
`Create` retains its sequence validation; `CreateTrusted` retains the same
length prefix, user-key bytes, seek kind, and packed trailer.

After backing allocation, the factory writes the complete varint prefix,
user key, and eight-byte trailer. No view escapes before encoding completes.
Allocation exceptions still propagate; partially constructed owners are not
published. No new catch, invalid-input fallback, storage format, or public API
is introduced.

Inline storage is still completely initialized, so moves need no new
indeterminate-byte rule or variable-length copy policy. Heap ownership and
the nine-byte canonical empty source remain unchanged. The added move-chain
characterization covers binary keys, prefix changes, inline/heap boundaries,
reused moved-from owners, and large keys before implementation.

## Experiments retained, not selected away

The original 84-process prototype removed inline initialization and copied
only the active prefix. Construction improved, but a 190-byte user key
(the full 200-byte inline representation) moved 32.98% slower in all six
pairs. A 120-process direct-move/memcpy refinement also regressed medium and
large inline moves, including approximately 14.68% to 22.49% for the
119-/120-/189-byte cases.

Those outcomes are retained. The selected implementation removes the
preliminary empty encoding and redundant heap initialization, but restores
the original initialized-inline representation and complete move code.
It does not add workload-specific key-size fast paths.

The earlier conditional Get signal belonged to the discarded broader
prototype, not to this narrowed change. It is not reused as this PR's
end-to-end performance claim.

## Final fixed-work local evidence

One benchmark source is compiled against each library's exact matching class
header. Both define the same 224-byte layout and public signatures; the
comparison does not mix conflicting class definitions across translation
units.

Before/after timed caller functions have identical complete machine-code
sections and relocation targets. The move constructor, move assignment, and
transfer helper also retain their exact original code and relocations:
276, 308, and 234 bytes respectively in this MSVC build.

Six balanced before/after rounds across ten cases produce 120 fresh
processes. Each executes 4,194,304 fixed operations after 262,144 warmup
operations, using owned CPU-2 affinity and the same toolchain/provider
configuration. Case order and before/after direction alternate by round.
There is no adaptive calibration or selected resampling.

A construction operation includes creation and owned destruction, including
heap allocation/free where applicable. A move operation is a roundtrip of
two move constructions and one assignment restoring one of 1,024
preconstructed owners; it allocates nothing in the timed loop.

The native pinned LookupKey is the encoding/view oracle outside timing.
Checks include empty and binary inputs, prefix transitions, inline/heap
boundaries, 16-KiB keys, canonical sources, self-move, and heap-address
stability. Timed checksums are independently recomputed from the original
encoding rules.

Wall costs below are medians of six process means in nanoseconds per
construction/drop operation:

| User bytes | Backing | Before | After | Cost change | Faster pairs |
|---:|---|---:|---:|---:|---:|
| 11 | inline | 18.847 | 16.603 | -11.91% | 6/6 |
| 190 | full inline | 21.059 | 18.416 | -12.55% | 6/6 |
| 191 | heap | 65.486 | 58.667 | -10.41% | 6/6 |
| 512 | heap | 69.680 | 60.151 | -13.68% | 6/6 |

Move roundtrip controls retain the following observations despite unchanged
move code; they are not presented as move improvements or causal guarantees:

| User bytes | Before ns | After ns | Change | Faster pairs |
|---:|---:|---:|---:|---:|
| 11 | 24.612 | 24.784 | +0.70% | 3/6 |
| 119 | 25.130 | 25.489 | +1.43% | 2/6 |
| 120 | 24.642 | 25.501 | +3.48% | 3/6 |
| 189 | 24.548 | 24.195 | -1.44% | 4/6 |
| 190 | 24.872 | 24.734 | -0.56% | 3/6 |
| 191 | 26.965 | 26.978 | +0.05% | 3/6 |

An independent verifier checks every command, stdout/result/sample, count,
affinity, native encoding checksum, input hash, header snapshot, median, and
paired delta in all three campaigns: 324 measured processes including the
rejected designs. The final isolated source matches the measured candidate.
All 54 normalized core compile commands match; 108 complete object/archive
member comparisons establish that only `format\internal_key.cc.obj` changes
runtime sections relative to the Hash-only baseline.

This is one shared Windows x64 host/compiler and process means, not tail
latency or a cross-platform cycle guarantee. Local CPU timing is quantized;
the elapsed construction measurements drive the stated result. Binary layout
and host variation remain relevant, especially for the unchanged-move controls.

## Validation and retained evidence

The final isolated branch passes all 976 local unit/public CTest cases,
including the repaired campaign process-group contract. Existing native
compatibility/model/crash gates remain required in CI, as do independent
review and changed-code coverage. No gate or timeout is relaxed.

The discarded prototype and first refinement are under
`build\optimization-delivery-20261022` and `build\lookup-copy1-20261022`.
Final raw input/source/class snapshots, caller and move-code identity, all
120 process records, and summaries are under `build\lookup-final-20261022`.
The independent audit and isolated source/archive proof are retained in
`build\optimization-delivery-20261022`.
