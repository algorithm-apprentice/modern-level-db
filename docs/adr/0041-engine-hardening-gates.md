# ADR-0041: Engine Hardening Gates

## Status

Accepted

## Context and prior art

ADR-0040 makes model, compatibility, crash, and fuzz verification repeatable.
The final `harden-engine` node turns the remaining checks into maintained
gates and records a performance baseline. It does not add product features or
declare the pre-alpha engine production-ready.

Google LevelDB's `db_bench` separates fill, random reads, and scans and
documents the operation count and value size. We adopt those bounded phases
without importing its large benchmark-option surface. Sanitizer and libFuzzer
drivers use the compiler runtimes directly; no new testing library is needed.

## Decision

### Sanitizers

Add `asan` and `tsan` configure/build/test presets. ASan/UBSan instruments C
and C++ and fails on the first sanitizer finding, including undefined
behavior that would otherwise only print a diagnostic. Its CTest selection
includes unit, model, compatibility, and crash tiers, not repeated nested
CMake configuration.

TSan is a separate build because it cannot be combined with ASan. Its gate
runs the concurrent component tests and ordered-map model. CI uses a macOS
Clang TSan runtime, which is already supported by the project's native
validation, rather than weakening host address-space randomization to work
around Linux shadow-memory placement. The Linux LLVM ASan job also covers
the upstream reference and real process-crash tests.

### Crash checks

Keep deterministic durable-state enumeration from ADR-0040. Add:

- Recovery from every byte-length prefix of a final, unsynced two-key WAL
  batch. Earlier synced data must survive; the new batch is visible either
  completely or not at all.
- A POSIX child that exits with `_exit` after acknowledged sync writes,
  bypassing database destruction. The parent waits for that specific child
  and verifies all acknowledged values after reopening.

These distinguish a torn tail and an abrupt process exit from orderly
shutdown; they do not claim to emulate every storage device or kernel crash.

### Fuzz budgets

PR and push CI keep fixed 1,000-run smoke campaigns. A weekly and manually
dispatchable workflow runs each target for 100,000 inputs with the same
15-second per-input timeout, 4 KiB input bound, 256 MiB single-allocation
limit, and 1 GiB RSS limit. That build instruments both codec dependencies
and the production library. Corpus and failure artifacts are uploaded, and
the workflow has a finite overall timeout.
Changes to the long-campaign workflow itself also run it on the pull request,
so its first installation and later edits are exercised before merging.

The longer workflow does not silently replace PR smoke coverage and is not
described as exhaustive. The gate driver is also invoked manually on this
branch before the node is declared complete.

### Benchmark baseline and severe-regression guard

Add an optional `MODERN_LEVELDB_BUILD_BENCHMARKS` target and a `benchmarks`
preset. Tests are not a dependency of this build; it reuses the existing
pinned LevelDB reference and codec setup.
The root configures that reference once when either the extended harness or
benchmark is enabled. A pre-existing parent `leveldb` target is rejected,
not silently substituted for the pinned oracle.

Measure both public APIs on the same machine, toolchain, seed, and options:

- 4,096 distinct keys and 256-byte values, partly compressible and partly
  pseudorandom, with Snappy, a 64 KiB write buffer, and 4 KiB data blocks.
- Random insertion, including opening and final shutdown, per inserted key.
- Random point reads after reopening and warming the cache, per read.
- Eight full scans, per yielded key.
- Three independent trials; alternate which implementation runs first.

Data generation and randomized orders are outside timed regions. Every read
and scan verifies its values, ordering, and count. Unexpected storage errors
or mismatches fail the benchmark rather than producing plausible timings.
Every trial uses fresh, owned temporary directories and destroys handles
before removing them.

Emit a versioned JSON report containing entries, trials, and every raw
nanoseconds-per-operation sample for `modern` and `leveldb`, grouped by
`write`, `read`, and `scan`. The checker requires the exact schema, positive
finite measurements, and the declared number of trials.

The median Modern LevelDB time for each phase must not exceed **20 times**
the reference median. This deliberately generous initial bound catches
catastrophic regressions without inventing a throughput SLA or overfitting
shared CI timing. It allows the documented durability and validation
differences. Reports retain actual ratios for future measured optimization;
changing the threshold requires updating this decision, not silently relaxing
CI after a slow run.

Checker tests include a passing report, the exact threshold, a just-over-limit
failure, missing/extra metrics, invalid trial counts, zero, negative,
non-finite, boolean, and nonnumeric samples. Benchmark gates run in Release
without sanitizer overhead.

## Scope and completion

Keep Linux/macOS/Windows Debug/Release and GCC 13 changed-code coverage gates.
Add Linux ASan/UBSan, macOS TSan, and Release benchmark jobs without disabling
existing checks. The harness's all-mode differential and power-loss jobs
remain mandatory.

No performance optimization is included without a demonstrated baseline
problem. No speculative recovery modes, compatibility migrations, repair
API, or extra production observability surface is introduced just to drive a
test. Any defect found here receives a minimal unit regression and normal
coverage review.

All canonical MVP DAG nodes are complete only after these gates and their
actual executions succeed. The project remains pre-alpha: bounded campaigns
and a clean review are evidence, not a guarantee against every storage bug.

## References

- [Google LevelDB benchmark](https://github.com/google/leveldb/blob/7ee830d02b623e8ffe0b95d59a74db1e58da04c5/benchmarks/db_bench.cc)
- [Clang AddressSanitizer](https://clang.llvm.org/docs/AddressSanitizer.html)
- [Clang ThreadSanitizer](https://clang.llvm.org/docs/ThreadSanitizer.html)
- [LLVM libFuzzer](https://llvm.org/docs/LibFuzzer.html)
