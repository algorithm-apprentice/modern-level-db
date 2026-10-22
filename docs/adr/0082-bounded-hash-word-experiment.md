# ADR-0082: Bounded Hash Word Experiment

- Status: Measurement-only

## Decision

Keep this candidate experimental. The implementation removes a redundant
checked-codec call from each complete hash word, but this campaign does not
establish a stable random-Get benefit or a reduction in the native-LevelDB gap.
Do not promote its raw median improvement as an accepted performance result.

The characterization and candidate commits are retained on the local experiment
branch; this record is not a production-adoption decision. No decoder,
cache-pin, LookupKey-initialization, API, ownership, or file-format experiment
is combined with it.

## Context

The existing `Hash32` processes every input byte and produces a 32-bit result.
Its complete-word loop called `ConsumeFixed32` after already establishing that
at least four bytes remain. In the sealed MSVC Release build, the call remains
out of line and returns a checked `Result`.

Native LevelDB revision `7ee830d02b623e8ffe0b95d59a74db1e58da04c5`
already inlines its unsigned little-endian byte assembly. The narrow hypothesis
is that removing Modern's additional checked-read machinery improves resident
random Get, not that a different hash algorithm or shorter cache identity is
needed. See [ADR-0054](0054-leveldb-cache-parity.md) for the cache baseline.

## Candidate and correctness boundary

Only `FoldHashWords` changes: assemble bytes zero through three into an unsigned
32-bit little-endian word, advance the borrowed input by four, and use the
existing mixer. The loop condition proves all four accesses are in bounds.
Byte-sized accesses require neither alignment nor pointer type-punning and do
not assume native endianness.

The multiplier, length contribution, seed, word mixing, tail, and public
checked codecs are unchanged. `Hash32` also serves serialized Bloom filters,
so preserving its output is a file-compatibility requirement, not merely a
cache-distribution preference.

Hash collisions already exist and remain permitted. Cache entries retain the
complete key; `Entry::Matches` requires equal hash **and** complete key bytes.
Lookup, replacement, growth, and erase preserve that rule. The candidate does
not replace the key with its hash, truncate the key, or skip the equality check.

Before changing production code, the experiment freezes native/Modern outputs
for every length from 0 through 4096, eight starting offsets, and five seeds:
163,880 comparisons. The candidate matches native and the complete frozen
baseline output digest. Literal native outputs for 8-, 16-, 17-, 31-, 32-, 33-,
255-, 256-, and 257-byte binary inputs are added as a characterization test
before the implementation change.

Both original and candidate pass the same 50 selected hash, sharded-cache,
Bloom-filter, and filter-block cases. These include the actual full-32-bit
collision pair `collision-3728` / `collision-161100`, not merely two keys in
the same bucket, and cover lookup, replacement, erase, growth, and pinned old
values. Testing supports the byte-equivalence argument; it does not exhaust
all arbitrary-length inputs.

## Frozen intervention

- Modern baseline: main `8cd23628ad10a8da7de872b8a323fe694f37db30`;
  its production tree matches measured `049ca681a55ccec42e7a382f6733e3fbd86021a9`.
- Pre-change characterization: `13a4bb4`.
- Measured production candidate: `3929f88`.
- Native: the sealed, project-pinned CRC-matched archive, not a newer upstream
  version or the portable-CRC reference configuration.

All 54 Modern core compiler commands match after build-directory
normalization. Complete object payloads match their archive members on both
sides: 108 comparisons. The other 53 objects have identical runtime sections
and relocation targets; only `base\hash.cc.obj` differs.

Modern's `Hash32` section changes from 183 to 174 bytes and loses its
`ConsumeFixed32` relocation. The candidate has no external relocations, and
its complete function bytes match the linked executable. This establishes
removal of the checked call, not a measured per-word cycle saving.

For the native control, an owned copy of `util\hash.cc` expands only its existing
inline word expression. Original and candidate `Hash` are identical 169-byte
functions without relocations, including in both linked executable images.
The candidate object is selected explicitly ahead of the unchanged native
archive.

This last choice moves native `Hash` from RVA `0x71990` to `0xb2e0`; Modern's
code and shared link layout also change. The native control is therefore an
instruction-identical hash, **not an identical executable**. The campaign does
not isolate the effects of placement, shared code, host variation, or their
interaction.

## Predeclared measurement

On October 22, 2026, run six four-arm rounds: Modern before/after and native
before/after, totaling 24 fresh measured processes. Each engine has three
baseline-first and three candidate-first pairs; every arm's mean position
within a round is 2.5. Four functional preflights are excluded from statistics.
All declared measured samples are retained; no favorable extra rounds are run.

Each process uses the same hash-verified, closed million-record image, copied
and verified independently. The common frozen driver object performs complete
ordered key/value verification, warmup, one million deterministic random Gets,
and complete verification afterward. Both engines retain mapped reads,
checksums, the same sealed CRC32C/Snappy/Zstd providers, and deferred background
work. The runner and inherited children are pinned to logical CPU 2.

The image has no Bloom filter. Capacity is 512 MiB; every process reports
270,423,944 charged cache bytes both before and after timing. The full-data
fingerprint is 2,355,815,558, timed checksum 114,131,307, and the seven-level
file-count/byte topology is unchanged. This campaign does not collect cache
visit counters or direct hash-throughput measurements.

## Results

Costs are process mean nanoseconds per Get; each summary is the median of six
fresh processes. Negative candidate deltas mean lower cost.

| Engine | Wall before | Wall after | Wall delta | Faster wall pairs | CPU before | CPU after | CPU delta |
|---|---:|---:|---:|---:|---:|---:|---:|
| Modern | 5,198.920 | 5,054.335 | -2.78% | 3/6 | 5,179.690 | 5,015.620 | -3.17% |
| Native no-op hash control | 4,740.340 | 4,492.255 | -5.23% | 6/6 | 4,687.500 | 4,492.190 | -4.17% |

The complete wall-time pairs are:

| Round | Modern before | Modern after | Modern delta | Native before | Native after | Native delta |
|---|---:|---:|---:|---:|---:|---:|
| 1 | 5,210.86 | 5,026.45 | -3.54% | 4,855.62 | 4,544.21 | -6.41% |
| 2 | 5,186.98 | 5,251.45 | +1.24% | 4,779.12 | 4,462.19 | -6.63% |
| 3 | 5,247.60 | 5,080.53 | -3.18% | 4,677.89 | 4,491.57 | -3.98% |
| 4 | 4,495.17 | 5,164.93 | +14.90% | 4,701.56 | 4,570.00 | -2.80% |
| 5 | 4,809.65 | 4,959.36 | +3.11% | 4,623.47 | 4,445.58 | -3.85% |
| 6 | 5,505.94 | 5,028.14 | -8.68% | 4,800.40 | 4,492.94 | -6.40% |

The observed Modern/native wall-cost gap is +9.67% before and +12.51% after.
The diagnostic ratio `(Modern_after / Modern_before) /
(Native_after / Native_before) - 1` is +2.59%; its CPU counterpart is +1.04%.
Neither quantity is a proven causal hash effect: the control itself improves
despite identical hash instructions, and executable placement is not fixed.
They do not prove the candidate causes a regression either.

An independent verifier reloads every raw command, stdout/result pair, sample,
order, count, fingerprint, topology, charge, input hash, and summary. It checks
all 21 frozen inputs and 51 immutable source-image files, verifies the
pre-change/candidate hash outputs and test reports, and recomputes medians,
all paired deltas, and both diagnostic ratios without calling the measurement
runner's summary routine.

## Consequences and retained evidence

The bounded word read is a justified implementation simplification with
preserved hash semantics. Its stable end-to-end benefit and contribution to
the random-Get gap remain unproved. A raw -2.78% median alone is insufficient
when only three Modern wall pairs improve and an instruction-identical native
hash control improves more consistently.

Do not infer a complete read-path root cause, generalize to other hosts or
compilers, combine this timing with a different campaign, or relabel the native
gain as a hash improvement. Any follow-up needs its own declared design and
must retain this negative-control result rather than resampling selected pairs.

Local evidence is retained under `build\cache-hash-ablation-20261022`:
the plan, original oracle, compiler audit, complete-member/code-generation
verification, functional preflight, all 24 process records, summary, independent
verification, and the two linked binaries. The separate candidate archive is
under `build\cache-hash-modern-20261022`; original sealed archives are untouched.
