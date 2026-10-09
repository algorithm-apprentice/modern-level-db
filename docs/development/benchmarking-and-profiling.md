# Benchmarking and Profiling

[Development guide](README.md)

Performance evidence is separated into:

1. ordinary comparative regression/baseline runs;
2. selected Google Benchmark workloads;
3. read diagnostics;
4. owned CPU capture.

Do not compare timings across different workloads, operation counts, access
policies, ownership modes, durability modes, binaries, or hosts.

## Ordinary comparative benchmark

POSIX:

```bash
cmake --preset benchmarks
cmake --build --preset benchmarks
ctest --preset benchmarks
```

The POSIX checker retains the exact 20x severe-regression gate.

Windows:

```text
cmake --preset windows-benchmarks
cmake --build --preset windows-benchmarks
ctest --preset windows-benchmarks
```

Windows copied and default-mapped runs are diagnostic-only. Each configuration
publishes separately named version-1 samples, final policy, and SHA-256 run
binding. Metadata cannot select a weaker checker mode.

| Preset | Samples | Final policy | Run binding |
|---|---|---|---|
| `benchmarks` | `build/benchmarks/benchmarks/results-Release.json` | `benchmark_policy-Release.json` in the same directory | `results-Release.provenance.json` |
| `windows-benchmarks`, copied | `build/windows-benchmarks/benchmarks/results-Release.json` | `benchmark_policy-Release.json` in the same directory | `results-Release.provenance.json` |
| `windows-benchmarks`, mapped default | `build/windows-benchmarks/benchmarks/results-mapped-Release.json` | `benchmark_policy-mapped-Release.json` in the same directory | `results-mapped-Release.provenance.json` |

## Selected workloads

POSIX:

```bash
cmake --preset profiling
cmake --build --preset profiling
ctest --preset profiling
```

Windows:

```text
cmake --preset windows-profiling
cmake --build --preset windows-profiling
ctest --preset windows-profiling
```

Native Windows selected-workload/reference runs require an ASCII absolute
build and output root because the pinned reference and Google Benchmark
command boundary remain narrow.

The selected cases are:

- `readrandom`, `readmissing`, `scan`, and `seek_reuse` at 4,096/65,536 records;
- overwrite, 32-key writebatch, writesync, and mixed50 at fixed work.

Run one case per new output directory:

```bash
python3 tools/run_performance.py \
  --binary build/profiling/benchmarks/modern_leveldb_performance \
  --case modern/readrandom/65536 \
  --output build/performance/modern-readrandom-64k
```

Native Windows uses the same case names:

```text
python tools\run_performance.py ^
  --binary build\windows-profiling\benchmarks\modern_leveldb_performance.exe ^
  --case modern/readrandom/65536 ^
  --output build\windows-performance\modern-readrandom-64k
```

Mutable cases reject adaptive `--min-time` and repeated measurements in one
process. Collect independent fresh processes in a predeclared alternating
order.

## Access and ownership controls

| Platform | Modern copied control | Reference copied control |
|---|---|---|
| POSIX | `--modern-file-access pread` | `--reference-file-access pread` |
| Windows | `--modern-file-access copied` | `--reference-file-access copied` when the authenticated build-owned helper is available |

Reference copied controls are valid only for `readrandom` and `readmissing`.
They are unavailable when
`FETCHCONTENT_SOURCE_DIR_MODERN_LEVELDB_REFERENCE` selects an external source
override; the default reference access remains available.

Modern point reads default to reusable caller output. Use
`--modern-result-ownership owning` only for the point-read ownership control.

Modern writebatch defaults to public copying behavior; the explicit
`--modern-write-batch-ownership exclusive` control calls `WriteExclusive`.
Mutable Modern cases can select `--modern-wal-creation leveldb` to match the
pinned reference's WAL-creation policy. The manifest records every policy.

## Current completion/report schemas

- Ordinary comparative samples: schema 1.
- Read-family selected-workload completion: schema 1.
- Fixed-count read diagnostics: schema 5.
- Mutable selected-workload completion: schema 3.
- Native Windows stack report: schema 2.
- Native Windows epoch ledger: schema 1.

Executable validators define the exact accepted fields, types, counts, and
cross-artifact identities. Historical schema descriptions remain in ADRs and
handoff documents and are not the current consumer reference.

## Read diagnostics

Use the instrumented executable only for attribution:

```bash
python3 tools/run_performance.py \
  --binary build/profiling/benchmarks/modern_leveldb_read_diagnostics \
  --case modern/readrandom/65536 \
  --read-diagnostics \
  --output build/performance/readrandom-64k-diagnostics
```

Windows:

```text
python tools\run_performance.py ^
  --binary build\windows-profiling\benchmarks\modern_leveldb_read_diagnostics.exe ^
  --case modern/readrandom/65536 ^
  --read-diagnostics ^
  --output build\windows-performance\readrandom-64k-diagnostics
```

The fixed foreground epoch records source decisions, file-open reasons, cache
outcomes, mapped/copied block bytes, decode/comparison counters, and sampled
inclusive stages. Diagnostic timings are not throughput evidence.

## CPU capture

macOS uses the supported Xcode Time Profiler path from the POSIX profiling
build.

```bash
python3 tools/run_performance.py \
  --binary build/profiling/benchmarks/modern_leveldb_performance \
  --case modern/readrandom/65536 \
  --output build/performance/modern-readrandom-64k-cpu \
  --capture-cpu
```

This requires an AppleClang build plus Xcode `xctrace` and `dsymutil`.

Native Windows capture requires explicit benchmark symbols and the owned
collector:

```text
python tools\run_performance.py ^
  --binary build\windows-profiling\benchmarks\modern_leveldb_performance.exe ^
  --case modern/readrandom/65536 ^
  --output build\windows-profile-readrandom ^
  --capture-cpu ^
  --native-symbols build\windows-profiling\benchmarks\modern_leveldb_performance.pdb ^
  --native-collector build\windows-profiling\benchmarks\modern_leveldb_windows_cpu_profile.exe
```

The collector launches only its own job-contained workload. It validates
benchmark PDB identity, thread ownership, measured epochs, calibration
coverage, module-relative stacks, cleanup, and raw artifact hashes. It does
not attach to arbitrary PIDs or start global tracing.

Capture perturbs execution. Captured-run timings are not throughput or speedup
evidence; use a separate ordinary run for timing conclusions.

## Artifacts and provenance

Every runner output directory contains a versioned manifest with:

- executable and relevant symbol/collector hashes;
- arguments and command logs;
- build/source/reference/dependency identity;
- workload/access/ownership/durability policies;
- validated raw artifact hashes for throughput and CPU-capture modes;
- cleanup status and failure diagnostics.

Never pair results from one run with a rebuilt binary or another policy file.
Failed or partially covered captures remain explicit non-success evidence.
Read-diagnostic manifests validate `read-diagnostics.json` but do not
currently bind that report with a manifest digest.

## Historical design evidence

- [Profiling research handoff](../profiling-design.md)
- [Write profiling handoff](../write-profiling-design.md)
- [Profiling foundation ADR](../adr/0042-benchmark-profiling-foundation.md)
- [Fixed-work write profiling ADR](../adr/0045-fixed-work-write-profiling.md)
- [Write-path parity ADR](../adr/0060-leveldb-write-path-parity.md)
- [Native Windows profiling ADR](../adr/0069-native-windows-selected-workload-profiling.md)
