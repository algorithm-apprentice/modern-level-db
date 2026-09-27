# ADR-0047: Inline Block-Iterator Key Reconstruction

## Status

Experiment completed; the candidate failed admission and was not committed.
The original vector-based iterator key remains in production.

## Evidence and prior art

ADR-0046 removed repeated checked entry decoding without weakening the block
construction boundary. Its new cache-fit read profile places 25.97% of CPU
sample weight in `Block::Iterator::ParseEntry`. Approximately 18.06% of the
whole process's self samples can be assigned to storage construction, copying,
allocation, and related vector operations beneath that call.

These are sample weights, not a guaranteed speedup. The current block
iterator owns a `vector<byte>` reconstructed key. Each newly created iterator
first allocates for a nonempty key, then uses generic reserve/resize/insert
operations while traversing prefix-compressed entries. Subsequent entries
already reuse vector capacity; do not misdescribe this as allocating on
every entry.

LevelDB uses a reusable string and explicit-length append. RocksDB's `IterKey`
uses reusable owned storage, a short inline buffer, and direct prefix/suffix
copies. A general-purpose small-vector dependency is unnecessary for our
single private caller.

An isolated storage probe established:

| Toolchain/library | Empty string capacity | 19-byte string inline? | Current iterator size |
|---|---:|---|---:|
| AppleClang/libc++ | 22 | Yes | 104 bytes |
| GCC 16/libstdc++ | 15 | No | 104 bytes |

The benchmark's internal keys are 19 bytes; a typical 16-byte user key plus
the eight-byte internal trailer is 24 bytes. Replacing the vector with
`std::string` would not consistently avoid these allocations across
libraries. Choose an explicit 32-byte private inline region, not a
platform-dependent string-capacity assumption.

## Decision

### Storage and scope

Change only the reconstructed key storage in `Block::Iterator`:

- A fixed 32-byte inline array.
- An optional owned heap byte array for larger keys.
- A mutable span viewing the active storage/capacity and a used-length field.
- Once allocated, retain the heap buffer and its capacity across shorter
  keys, seeks, invalidation, and direction changes.

The iterator remains non-copyable and non-movable. Its active-storage span
can therefore safely refer to its own inline array. A block move does not
move its iterators or their key storage. The expected iterator footprint
on the probed 64-bit platforms grows from 104 to 144 bytes; that is an
explicit per-live-iterator tradeoff, not a change in block-cache charge.

Keep this storage inside the iterator, without a generic container, shared
allocator, configuration knob, or new public API. `LookupKey` already has
inline storage and is not part of this experiment.

### Reconstruction and failure atomicity

The validated entry supplies `shared` prefix length and a key-delta view.
ADR-0046's invariants establish that the prefix is within the previously
reconstructed key and that the new length fits the owning block's addressable
extent.

For each entry:

1. Compute `required = shared + delta.size()` in `size_t`.
2. If required capacity exceeds the active buffer, allocate an uninitialized
   owned byte array for exactly the requested size, preserving the existing
   requested-size reserve policy. Copy only the shared prefix into it before
   replacing the old owned buffer and active span.
3. Copy the delta directly into active storage at offset `shared`.
4. Set the used length and advance iterator state as before.

Allocation is the only new fallible step, and it happens before changing
storage or iterator state. Prefix and suffix copying afterward cannot
allocate. Do not grow by unchecked multiplication, zero-fill bytes that are
immediately overwritten, or copy unused capacity.

The inline array need not be initialized wholesale. Its used length starts
at zero; a restart has shared length zero; all newly exposed bytes are
written before `key()` can expose them. Reuse reads only the established
prefix. Deleted iterator copying/moving prevents accidental memberwise
copies of uninitialized spare bytes.

Copy sources are the prior active key and the immutable block delta. A new
allocation is distinct from the prior buffer; the block's bytes are distinct
from iterator-owned key storage. Zero-length copies use valid storage/block
addresses. No source view is retained after its owner may be released.

### Every relevant iterator surface

`key()` must return the active prefix, not the old vector. `Seek` must compare
that same current key. `SeekToRestartPoint` and `Invalidate` reset used length,
not capacity or heap ownership. Forward/reverse traversal and key/value view
lifetimes keep their existing contracts.

Preserve allocation-before-state-mutation behavior and the documented
consistent, unspecified position after an exception. Keep binary keys,
embedded NULs, empty keys, reverse/custom ordering, long keys, block moves,
and all accepted varint encodings unchanged.

Do not modify checked block validation, the validated decoder, comparator
logic, value ownership, cache behavior, I/O, CRC, persisted bytes, or benchmark
workloads. In particular, the block validator's own temporary key vectors
remain outside this experiment.

## Focused verification

Add fast, memory-only tests before implementation:

- Short reconstructed keys really reside inside the iterator object, with
  checks at 0, 19, 31, and 32 bytes. This is the explicit inline-storage
  requirement, not an assumption that fewer source lines imply fewer allocations.
- A 33-byte key spills correctly, including a 32-byte shared prefix.
- Subsequent larger keys grow safely; smaller keys and seeks retain the
  already allocated heap buffer.
- Restart entries can trigger a spill with a zero shared prefix.
- Direction changes and restart transitions work on both sides of the
  inline boundary, preserving exact key/value bytes.
- Existing ordered-model, empty/binary-key, reverse-comparator,
  noncanonical-varint, and block-move tests remain unchanged and pass.

Object-address containment is permitted in these private storage tests;
avoid process-global allocation hooks or heavyweight new test infrastructure.
Keep the cases small and without filesystem I/O or sleeps.

Use the existing native unit, sanitizer, differential/model/crash, fuzz,
platform, and changed-code coverage gates. Inspect optimized code and matched
profiles to check that the short-key path avoids heap allocation and the
generic vector construction/insert calls.

## Measurement and admission

Reuse ADR-0046's fixed measurement design, but freeze a new clean baseline
after this design merges; it must already include the validated decoder.
Keep the candidate local and uncommitted until correctness and performance
admission pass, retaining its exact source patch, base revision, build
metadata, binary hashes, and honest dirty-worktree provenance.

Fix three paired rounds in advance, with three individual repetitions and
a 0.3-second calibration minimum per fresh process. Run all eight Modern
read-family cases, baseline then candidate in rounds one and three and the
reverse order in round two. After each round, collect the two LevelDB
random-read sizes as reference context: 54 processes in total.

Require:

- At least 5% higher median Get/s for `modern/readrandom/4096`, with positive
  improvement in each paired round.
- No other Modern read case loses more than 5% aggregate median throughput
  or loses more than 5% in at least two rounds.
- Correctness and the final delivery gates pass.

Use medians of individual `1e9 / wall_ns_per_item` values; report wall and
process CPU time alongside throughput. Preserve all samples and workload
definitions; do not add favorable rounds or tune the inline size after
seeing results. These are single-foreground-thread results, not P99 or
concurrent saturation capacity, and do not claim universal gains for long
keys or every architecture.

If the candidate passes, capture the exact frozen binaries for cache-fit
random-read CPU attribution, then commit/push the accepted source for final
CI and delivery. If it fails, restore production and record the outcome,
retaining only tests applicable to the retained implementation.

## Outcome

Design-only PR #48 merged at `0f91137`. The candidate remained uncommitted
through correctness checks, the fixed throughput experiment, and one
post-measurement diagnostic CPU capture. No candidate implementation commit
was made or pushed.

| Artifact | SHA-256 |
|---|---|
| Clean original executable | `c9a87f6b676780c5a3daf13c86f81e70a701f9e7fee05345c0705809b82ac6b3` |
| Candidate executable | `562ff2d4331c561f1552a6294a43cbd437209adc04ac9319b6d98d07ee165042` |
| Candidate source patch | `8193680552e565abb3403ae2ac8f39d5d16171c8f543fbd591062a4f01521e6a` |

The candidate passed 547 native unit cases, 557 sanitizer/model/differential/
crash cases, and bounded Sol implementation review. Its 144-byte iterator
footprint was confirmed with both local standard libraries. Storage tests
proved short keys were inline, and optimized code no longer called vector
reconstruction machinery. Correctness and simpler generated code were not
substituted for the performance gate.

All 54 predetermined processes completed. The nine individual samples per
Modern variant/case gave:

| Case | Original items/s | Candidate items/s | Throughput gain |
|---|---:|---:|---:|
| `modern/readrandom/4096` | 2,126,602 | 2,137,891 | 0.53% |
| `modern/readrandom/65536` | 664,323 | 676,577 | 1.84% |
| `modern/readmissing/4096` | 2,249,112 | 2,239,713 | -0.42% |
| `modern/readmissing/65536` | 688,010 | 707,554 | 2.84% |
| `modern/scan/4096` | 22,861,086 | 24,530,608 | 7.30% |
| `modern/scan/65536` | 7,515,884 | 7,768,707 | 3.36% |
| `modern/seek_reuse/4096` | 415,458 | 473,811 | 14.05% |
| `modern/seek_reuse/65536` | 197,401 | 228,325 | 15.67% |

The primary gains by round were 1.11%, 0.28%, and -0.55%. It fails both the
5% aggregate threshold and the requirement for positive gains in every
round. Controls do not trigger rejection, but their improvements cannot
replace the predeclared primary target. In particular, the 7.30% cache-fit
scan gain does not justify changing the objective after measurement.

The reference processes measured 2,469,320 and 973,277 Get/s for their two
random-read sizes. These provide context only; the candidate did not
materially close the cache-fit read gap.

One additional candidate-only CPU capture, not a new throughput run,
contained 6,996 samples with no symbol-generation warnings. It placed
15.32% inclusive sample weight in entry parsing and 47.31% in internal-key
comparison. The work distribution changed, but this does not override the
end-to-end result or prove exactly which costs offset the expected benefit.
Do not conclude that inline storage is universally ineffective.

Restore `block.h` and `block.cc` exactly to their pre-experiment state and
rebuild the normal binaries. Remove assertions requiring the rejected inline
layout; retain only general growing/shrinking-key and binary-restart behavior
regressions. The restored implementation passed 546 native unit cases and
23 sanitizer block/model cases.

The diagnostic review also found symbol-generation warnings in ADR-0046's
older frozen-baseline capture, after its shared build objects had been
replaced. That earlier fine-grained CPU comparison is now qualified in its
outcome; the unprofiled throughput results are unaffected. The warning-free
candidate capture used to select this experiment remains a valid observation.
Future frozen-baseline CPU collection must retain matching build objects or
explicitly preserved symbols, as documented in the profiling guide.

Raw samples, the rejected source patch, frozen binaries, code-generation
evidence, and the diagnostic capture remain local under
`build/inline-key-optimization/`. Do not tune capacity or add favorable
rounds to rescue this candidate.

## Delivery boundary

Obtain one bounded Sol design review and merge this design-only PR before
code. Review the local candidate before measurement; implementation is
committed only after local admission. Keep lazy file selection, merge-cursor
caching, full-block validation changes, mmap, concurrent throughput work,
and CI test-job restructuring separate.

## References

- [ADR-0046 validated decoding and measurement discipline](0046-validated-block-decoding-experiment.md)
- [ADR-0022 immutable block and iterator contracts](0022-sstable-block-format.md)
- [LevelDB key reconstruction](https://github.com/google/leveldb/blob/7ee830d02b623e8ffe0b95d59a74db1e58da04c5/table/block.cc)
- [RocksDB IterKey storage](https://github.com/facebook/rocksdb/blob/abeebd9630f11bd08c28b7bd43c7bdfc62050654/db/dbformat.h)
