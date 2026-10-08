# ADR-0067: Windows Comparative Benchmark Baseline

## Status and scope

Proposed on 2026-10-08 after native diagnostics PR #103.
This is the final first-release delivery slice,
`baseline-windows-performance`, in ADR-0064. Independent measurement/storage
and Windows/build/report-contract reviews precede implementation.

Admit the existing ordinary comparative database benchmark on native x64/MSVC
Windows. Preserve Linux/macOS workloads, report schema, and severe-regression
admission behavior. Do not port the selected-workload CPU profiler, process
collector, POSIX pread control, mapped reads, or read-instrumented harness
implicitly by lifting a shared capability guard.

## Existing callers and evidence

`benchmarks/db_bench.cc` uses deterministic records and shuffled order,
alternates engine execution order, validates reads/scans, and reports three
phases over odd repeated trials. Write timing includes create/write/close;
read timing follows reopen and warm reads; scan timing performs eight passes.
It is an ordinary baseline, not a sync-write latency SLA or a cold-cache test.

The checker validates the exact version-1 report and positive finite samples,
uses medians, and rejects slowdown strictly greater than 20x. Its default
behavior and existing exact-threshold tests must remain unchanged.

The private reference is pinned original LevelDB, with the Windows Env and
target-local RTTI/codec linkage already verified by ADR-0065. The native
Modern backend is copied-read and has explicitly weak namespace durability.
The reference retains its platform-default file access and native durability
policy. Those policy differences cannot be hidden by a shared workload.

## Decision 1: Reuse the ordinary workload, not unsupported capabilities

Allow MODERN_LEVELDB_BUILD_BENCHMARKS on the admitted native backend.
Keep MODERN_LEVELDB_BUILD_PERFORMANCE_TESTS Windows rejection until a separate
design admits its instrumentation, collector, provenance, and process
lifecycle. Existing POSIX flags and capture capabilities remain unchanged.

Modern benchmark options explicitly set allow_weak_namespace_durability=true
on Windows, without changing the public default. Use the validated ASCII
ReferenceTemporaryDirectory for both engines' disposable trial paths.
This avoids the original reference's ANSI path limitation while leaving
Modern-only Unicode verification intact.

Keep records, seed, alternating order, phase timing boundaries, warm-up,
scan passes, entry/trial bounds, correctness checks, and version-1 sample
JSON exactly as the existing workload. Do not optimize algorithms or choose
different Windows record counts merely to improve a comparison.

Executable identity in CTest/runner comes from TARGET_FILE; do not append
or assume .exe in Python paths. Use ordinary bounded subprocess.run execution
for this benchmark, not ps/pgrep, POSIX signals/groups, or CPU collection.
Subprocess/schema/write failures remain explicit errors.

## Decision 2: Publish policy provenance without changing legacy sample schema

Keep the benchmark stdout report at schema_version=1:
entries, trials, and samples for modern/leveldb write/read/scan.
Generate an adjacent build-owned `benchmark_policy.json` sidecar from actual
CMake target/backend selection, with a separate policy schema version.
It is not included in timing and is not a database file.

Required provenance:

- platform, compiler id/version, and admitted target architecture;
- pinned reference revision or verified explicit source-override identity;
- Modern file access and namespace-durability policy;
- reference file access as platform_default, not falsely forced-pread;
- Modern sync_wal_creation setting and unsynced workload-write policy;
- sample schema version and performance admission policy.

On Windows the sidecar states native copied reads, explicit weak namespace
consent, reference platform-default reads, and diagnostic-only performance
evaluation. On Linux/macOS retain existing default access/durability and
severe-regression evaluation. Do not include developer identity, machine
paths, credentials, or an invented hardware/cold-cache claim.

Source overrides must not claim the pinned identity merely because an option
name says reference. Verify available revision/provenance or mark it external/
unknown. The already provided matching local revision can be checked during
validation without committing its path.
For a git source override, record the actual revision and whether tracked or
untracked source changes exist; a pinned HEAD with changes is modified/external,
not clean pinned provenance. An authenticated fallback archive records its
verified pinned download identity separately from override provenance.

The trusted runner writes an adjacent run-binding manifest after successful
execution/validation. It includes SHA-256 identities of the executed binary,
sample report, and policy sidecar, plus build configuration and expected policy.
Do not pair a stale sample file with a new build's sidecar and call that
provenance; only artifacts from the same bound run are published together.

## Decision 3: Separate Windows baseline collection from performance admission

Add an explicit checker CLI diagnostic-only mode. This mode still validates
the complete sample schema, all sample counts, positive finite values,
both engines, and all three phases, then reports median slowdown ratios.
Ratios must also remain finite: finite input samples can overflow during
division, and diagnostic mode must reject that invalid measurement too.
It does not reject a valid Windows baseline for crossing the existing 20x
floor, since backend policy/performance parity is not yet established.

Default checker calls retain exactly the existing severe-regression gate.
The report or sidecar must not silently downgrade admission: only the
explicit trusted caller/CTest argument selects diagnostic-only evaluation.
An invalid report remains failure in either mode; no missing result becomes
a successful diagnostic record.

CMake selects diagnostic-only arguments only for admitted native Windows.
The existing Linux/macOS benchmark test name/threshold behavior remains.
Name the Windows run truthfully as a native baseline/contract check rather
than a passed severe-regression admission experiment. Persist both samples
and policy sidecar as artifacts.

This is not a claim that a >20x result is good, that policies are equivalent,
or that the first Windows implementation is optimized. It establishes
repeatable correctness and measurement before a later optimization ADR.

## Build, tests, and review gate

Add a Windows Release benchmark preset and native CI baseline job with the
MSVC developer environment. Preserve dependency isolation, codec/reference
target boundaries, disabled-feature consumers, documentation-only routing,
and nonempty test selection.

Observe Windows ordinary benchmark configuration fail under the current
POSIX-only guard before admission. Observe a diagnostic-only checker test
fail before adding that mode. Required exit evidence:

- MSVC Release ordinary benchmark builds and runs the same default workload.
- All validated read/scan values and operation/sample counts remain exact.
- Existing checker invalid-schema/count/value/engine/phase cases and exact
  twenty-times threshold tests pass unchanged for default policy.
- A finite above-threshold report is accepted only with explicit diagnostic
  mode; malformed/NaN/zero/missing reports still fail in that mode.
- Overflowing ratios fail unconditionally. Default mode still rejects >20x
  even beside diagnostic sidecar metadata; provenance never selects a policy.
- Native runtime stdout remains valid version-1 JSON and native sidecar
  matches actual compiler/backend/reference selection and policy.
- Windows profiling/instrumented capture remains explicitly unsupported.
- Existing POSIX benchmark, profiling, coverage, sanitizer, fuzz, and
  compatibility checks stay green; no threshold or report gate is disabled.
- Independent measurement/storage and Win32/build/report-contract reviewers
  assess exact implementation and focused fixes before autonomous merge.

The first release is complete only after the design, native filesystem,
opted-in database, extended recovery/compatibility, diagnostic command,
and ordinary benchmark slices are verified and persistent on main.
Mappings and CPU profiling remain explicitly separate future goals.

## Design review record

Independent measurement/storage and Windows/build/report-contract personas
reviewed the design. Three measurement gaps were accepted: same-run artifact
binding, modified source-override provenance, and unconditional finite ratios
plus tests proving metadata cannot downgrade default admission.
The focused closure confirmed those corrections without changing legacy
sample shape, explicit caller policy selection, capability separation, or
durability/timing nonclaims. No actionable finding remains from these passes.

ASCII English, LF/whitespace, fences, local references, DAG ordering, and
documentation-only scope were checked. This design contains no runtime/gate
change and claims no new benchmark result.

## References

- [ADR-0006 sequential review](0006-sequential-pull-request-workflow.md)
- [ADR-0041 original hardening/benchmark gate](0041-engine-hardening-gates.md)
- [ADR-0064 first Windows release](0064-windows-filesystem-and-delivery.md)
- [ADR-0065 pinned reference and fixtures](0065-windows-recovery-and-compatibility-verification.md)
- [Ordinary comparative workload](../../benchmarks/db_bench.cc)
- [Existing checker](../../tools/check_benchmark.py)
- [Exact-threshold tests](../../tests/tools/benchmark_gate_test.py)
