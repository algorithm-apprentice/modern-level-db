# ADR-0059: Pinned LevelDB Hardware CRC32C Control

## Status

Completed measurement-only control. Bounded GPT-5.6 Sol review required an
explicit final link-property contract, externally selected provenance
expectations, runtime ARM64-dispatch evidence, and a retained matrix
validator. The measured variant implemented those requirements without
changing the canonical reference or production code.

## Context

The canonical benchmark reference is Google LevelDB revision
`7ee830d02b623e8ffe0b95d59a74db1e58da04c5`. The repository deliberately
builds it with:

```cmake
set(HAVE_CRC32C 0)
```

so LevelDB uses its internal portable checksum implementation. Modern LevelDB
links Google CRC32C revision
`2bbb3be42e20a0e6c0f7b39dc07dc863d9ffbc07`; the frozen macOS ARM64 builds
record `crc32c_compiled_arm64=true`.

[ADR-0053](0053-leveldb-read-path-parity.md) and
[ADR-0058](0058-4k-hot-set-fixed-cost-alignment.md) therefore compare Modern's
hardware-dispatched checksum path with a pinned LevelDB reference whose
optional external acceleration is disabled.

That comparison is reproducible and remains the canonical parity baseline,
but it does not answer how Modern compares with the same pinned LevelDB source
linked to the same Google CRC32C implementation.

Historical Modern before/after evidence in
[ADR-0044](0044-profile-guided-crc32c-acceleration.md) shows:

| Workload | Hardware CRC wall reduction |
|---|---:|
| `readrandom/4096` | 0.88% |
| `readmissing/4096` | 1.81% |
| `readrandom/65536` | 44.78% |
| `readmissing/65536` | 44.23% |
| `scan/65536` | 51.84% |

Those numbers identify the likely scale but cannot be transferred directly to
LevelDB because its table/cache/decompression costs differ.

## Decision

Build and measure a separately identified hardware-CRC-enabled variant of the
same pinned LevelDB revision. This is an attribution control only:

- Do not change the canonical `HAVE_CRC32C=0` reference.
- Do not change ADR-0053 or ADR-0058 admission decisions.
- Do not merge the reference patch into production CMake.
- Do not call the result a new project baseline without a separate ADR.

## Reference build patch

Use a clean detached worktree at current `main`. Apply one local,
measurement-only patch that:

1. Sets `HAVE_CRC32C 1` before adding pinned LevelDB.
2. Replaces the final post-`add_subdirectory` link properties with values that
   explicitly retain the exact `${MODERN_LEVELDB_CRC32C_TARGET}` already
   linked by Modern:

   ```cmake
   LINK_LIBRARIES:
     snappy;zstd;crc32c;Threads

   INTERFACE_LINK_LIBRARIES:
     snappy;zstd;$<LINK_ONLY:crc32c>;Threads
   ```

   This is necessary because the harness's existing property override would
   otherwise erase upstream's conditional bare `crc32c` link.
3. Retains the existing Snappy, Zstd, Threads, RTTI, mmap-control patch, and
   pinned LevelDB source revision.
4. Exposes a generated provenance value:

   ```text
   reference_hardware_crc=google-crc32c-arm64-v1
   ```

   rather than the canonical:

   ```text
   reference_hardware_crc=disabled
   ```

5. Uses a predeclared matrix driver with externally selected expected roles.
   For every completed invocation it requires the exact:
   - executable hash;
   - configure revision and dirty state;
   - reference hardware-CRC marker;
   - CRC target, provider, requested revision, and ARM64 compiled capability.

   It rejects a binary that merely labels itself hardware-enabled.

The patch must not edit LevelDB's checksum source, table reader, block format,
or benchmark adapter. LevelDB's existing `port::AcceleratedCRC32C` path calls
Google CRC32C `Extend`.

## Build verification

Before timing, prove:

- Candidate `port_config.h` defines `HAVE_CRC32C 1`.
- Canonical baseline `port_config.h` defines `HAVE_CRC32C 0`.
- Candidate LevelDB links the same Google CRC32C target and requested
  revision as Modern.
- The LevelDB compile command contains Google CRC32C's include directory.
- The final executable link command contains `libcrc32c`.
- Candidate build provenance records:
  - provider and source directory;
  - requested Google CRC32C revision;
  - `crc32c_compiled_arm64=true` on the measurement host;
  - `reference_hardware_crc=google-crc32c-arm64-v1`.
- The canonical binary records `reference_hardware_crc=disabled`.
- Candidate and canonical binaries have different SHA-256 hashes.
- Both source worktrees, patches, compile commands, dirty state, and binary
  hashes are retained.
- The CRC provider is `pinned-source`, the source override is empty, and the
  exact local measurement patch has a retained SHA-256.

Run the benchmark contract tests and at least one LevelDB database
compatibility/smoke case before measurement. The hardware candidate must read
the identical corpus and produce the identical completion fingerprints.

Capture one checksum-heavy hardware LevelDB CPU profile and require a resolved
`crc32c::ExtendArm64` frame. Compiled ARM64 support and linked symbols alone
do not prove runtime dispatch.

## Fixed measurement matrix

Use clean, separate build directories and never rebuild frozen binaries
between rounds.

Use three paired rounds, three framework repetitions per fresh process, a
0.2-second minimum time, and alternating pair order in round two.

### Hardware effect inside LevelDB

Compare hardware-enabled LevelDB against canonical disabled-CRC LevelDB for:

- `readrandom/4096`
- `readmissing/4096`
- `readrandom/65536`
- `readmissing/65536`

in both:

- default mmap;
- forced pread.

This is eight cases, 48 fresh processes.

### Cross-engine measurement matrix

From the hardware-enabled candidate executable, compare Modern with its
hardware-enabled LevelDB adapter for the same eight cases and protocol.

This adds 48 fresh processes. The complete control therefore contains 96
fresh-process invocations and 288 individual benchmark repetitions.

Using one candidate executable for this cross-engine pair ensures the compiler
flags, dependencies, corpus, and host state match.

## Aggregation

Use a retained driver/manifest that predeclares all 96 unique
role/access/workload/size/round cells, pair order, frozen hashes, expected
markers, and repetition count. It must finish with exactly 96 complete unique
commands and no running, duplicate, or omitted cell.

For each invocation use the three individual `wall_ns_per_item` and
`process_cpu_ns_per_item` values.

- A round value is the median of that invocation's three repetitions.
- A round delta is `left / right - 1`.
- An aggregate value is the median of all nine individual repetitions.
- Hardware-effect delta is `hardware / disabled - 1`.
- Cross-engine delta is `Modern / hardware-LevelDB - 1`.

Report:

1. LevelDB hardware versus LevelDB disabled.
2. Modern versus LevelDB hardware.
3. Wall time as the primary descriptive metric.
4. Process CPU as supporting evidence.

There is no admission threshold. This control measures configuration impact;
it neither accepts nor rejects production code.

## Outcome

### Frozen build identity

Both builds use source revision
`6f0ff5604ccddd0ffb81abf424d630bdd500d0f6`.

```text
hardware-reference patch SHA-256:
bf909915b1303b7082aa3b1eb5914685efc5a410dcb6911393132cda634b383a

hardware-reference executable SHA-256:
eaed685e581f34f4d6bcec5613e97ed36db67768489113ad81fa6ff6af79206d

canonical-disabled executable SHA-256:
0fa9711436e056667c4206bcc4eb2767626309f868ff6bd789a9727f4085f39b
```

The hardware report records:

```text
reference_hardware_crc=google-crc32c-external-v1
crc32c_target=crc32c
crc32c_provider=pinned-source
crc32c_requested_revision=2bbb3be42e20a0e6c0f7b39dc07dc863d9ffbc07
crc32c_compiled_arm64=true
```

The generated LevelDB `port_config.h` contains `HAVE_CRC32C 1`; its compile
command contains Google CRC32C's include directory; the final executable link
contains `libcrc32c.a`. The canonical build retains `HAVE_CRC32C 0` and
`reference_hardware_crc=disabled`.

A retained checksum-heavy LevelDB `readrandom/65536` CPU capture contains
7,494 samples with no low-confidence warning. It resolves
`crc32c::ExtendArm64` at 8.51% inclusive and 0.67% self sample weight, proving
runtime ARM64 dispatch rather than merely linked support.

### Host-stability qualification

An initial continuous matrix ran while another foreground Copilot process
consumed CPU. The same canonical LevelDB binary measured about 614 ns/Get
inside that run and 402 ns/Get immediately afterward. That matrix is excluded.

The retained 4,096-record results use a full three-round matrix with a
five-second cooldown before every process. The retained 65,536-record results
use a bounded stability confirmation with:

- three paired rounds;
- five repetitions per invocation;
- one-second minimum time;
- five-second cooldown before every process.

No sample was removed within either retained set.

### Hardware CRC effect inside LevelDB

Positive wall reduction means hardware CRC is faster.

| Access | Workload | Hardware ns/op | Disabled ns/op | Wall reduction |
|---|---|---:|---:|---:|
| mapped | `readrandom/4096` | 414.91 | 413.12 | -0.43% |
| mapped | `readmissing/4096` | 425.89 | 416.15 | -2.34% |
| copied | `readrandom/4096` | 428.99 | 422.04 | -1.65% |
| copied | `readmissing/4096` | 415.20 | 427.30 | +2.83% |
| mapped | `readrandom/65536` | 901.78 | 1002.20 | +10.02% |
| mapped | `readmissing/65536` | 846.31 | 1009.49 | +16.16% |
| copied | `readrandom/65536` | 1144.83 | 1238.35 | +7.55% |
| copied | `readmissing/65536` | 1081.23 | 1253.09 | +13.72% |

The 4,096-record differences are small and inconsistent, as expected for a
cache-fit workload. Hardware CRC materially improves all stabilized
65,536-record aggregates, by 7.55% through 16.16%.

### Measured Modern versus hardware-enabled LevelDB

Positive delta means Modern is slower.

| Access | Workload | Modern ns/op | Hardware LevelDB ns/op | Delta |
|---|---|---:|---:|---:|
| mapped | `readrandom/4096` | 417.59 | 426.90 | -2.18% |
| mapped | `readmissing/4096` | 415.27 | 424.57 | -2.19% |
| copied | `readrandom/4096` | 417.33 | 410.24 | +1.73% |
| copied | `readmissing/4096` | 403.68 | 405.02 | -0.33% |
| mapped | `readrandom/65536` | 939.76 | 955.00 | -1.60% |
| mapped | `readmissing/65536` | 885.03 | 854.22 | +3.61% |
| copied | `readrandom/65536` | 1216.75 | 1109.38 | +9.68% |
| copied | `readmissing/65536` | 1191.79 | 1160.20 | +2.72% |

Modern and hardware-enabled LevelDB remain close in cache-fit and default-mmap
cases. The clearest gap is forced-pread `readrandom/65536`, where Modern is
9.68% slower. This control therefore narrows the strongest interpretation of
ADR-0053: Modern matches the canonical disabled-CRC reference and remains
close to the hardware-enabled reference in default mmap, but does not match
it in every copied-read pressure case.

The effect is much smaller than Modern's historical 44% CRC acceleration
because this LevelDB version's total measured work includes other cache,
decompression, and lookup costs, and its portable checksum baseline differs.

## Interpretation

Expected interpretation boundaries:

- The 4,096-record cases should show little checksum sensitivity because hot
  data blocks are normally served from cache.
- The 65,536-record cases include block reads, decompression, and checksum
  verification and may show a material LevelDB gain.
- A faster hardware-enabled LevelDB does not invalidate Modern's correctness
  or the structural parity work.
- It does narrow or reverse the performance comparison against the fastest
  measured pinned configuration and must be stated explicitly.
- Results apply to this ARM64 host, corpus, cache size, codecs, and
  single-foreground-thread workload.

Do not infer x86 results or universal storage-engine rankings.

## Validation and review

- Bounded GPT-5.6 Sol design review before the patch.
- Fresh configure/build and benchmark contract tests.
- Exact provenance and symbol/link verification.
- All 96 predetermined invocations complete successfully.
- Reject missing, mislabeled, mixed-revision, or mixed-binary artifacts.
- Bounded result review before documenting the outcome.

## Delivery

Use two sequential documentation-only PRs:

1. This design.
2. The measured outcome with exact revisions, patch/binary hashes, aggregate
   and per-round results, and interpretation.

Keep raw reports and the measurement patch in local evidence storage.

## Consequences

- The canonical parity baseline remains reproducible and unchanged.
- The project gains an honest comparison against pinned LevelDB's optional
  hardware checksum configuration.
- Hardware CRC has negligible 4 KiB impact and a material 65 KiB impact.
- Default-mmap Modern remains close to hardware-enabled LevelDB; copied
  pressure reads expose a remaining gap.
- Future performance statements can distinguish:
  - canonical pinned LevelDB;
  - hardware-CRC-enabled pinned LevelDB;
  - Modern LevelDB.

## References

- [ADR-0044 Modern hardware CRC32C acceleration](0044-profile-guided-crc32c-acceleration.md)
- [ADR-0053 completed pinned read-path parity](0053-leveldb-read-path-parity.md)
- [ADR-0058 4 KiB fixed-cost alignment](0058-4k-hot-set-fixed-cost-alignment.md)
- [Pinned LevelDB CRC32C implementation](https://github.com/google/leveldb/blob/7ee830d02b623e8ffe0b95d59a74db1e58da04c5/util/crc32c.cc)
- [Pinned LevelDB port acceleration hook](https://github.com/google/leveldb/blob/7ee830d02b623e8ffe0b95d59a74db1e58da04c5/port/port_stdcxx.h)
- [Google CRC32C revision](https://github.com/google/crc32c/tree/2bbb3be42e20a0e6c0f7b39dc07dc863d9ffbc07)
