# ADR-0069: Native Windows Selected-Workload Profiling

- Status: Implemented

## Status and scope

Design accepted and merged by PR #108 on 2026-10-08 after native-reference
mapped-read PR #107. The native selected-workload/profiling slice is
implemented. Independent measurement/storage and Windows/process/symbol
design and code reviews gate sequential delivery.

Complete `implement-windows-profiling` from ADR-0064. Reuse existing selected
workloads, exact correctness/completion checks, explicit ownership/file-access/
WAL controls and private pinned dependencies. Admit only native MSVC x64
desktop Windows on the existing local NTFS/weak-namespace contract.
Do not invent new database algorithms or infer performance equality from
similar API calls. Residual differences require same-machine measurements and
hot-path attribution, following the owner's native-reference-first direction.

## Current callers and primary evidence

The ordinary native benchmark now compares explicit copied and default-mapped
paths against original LevelDB, with separate bound artifacts. Larger local
read/scan observations improved from approximately 3.23x/3.09x to 1.14x/1.16x.
The smaller workload still showed a larger point-read gap. These are diagnostic
observations, not an explained universal speedup or product admission.

The selected-workload harness already implements deterministic readrandom,
readmissing, scan, retained seek, overwrite, writebatch, writesync and mixed
cases. Its normal throughput executable excludes read instrumentation.
The separate diagnostic executable counts mapped/copied blocks and setup
file-open reasons. Current CPU collection is macOS-specific and uses signposts,
process groups and Xcode; native Windows must not fall through those APIs.

Microsoft documents GetThreadTimes, suspended-thread GetThreadContext and
StackWalk64/SymFromAddr. DbgHelp calls are single-threaded. Thread CPU times
have 100 ns units; they are not wall time or exact per-instruction durations.
A native owned-child probe, compiled optimized with debug symbols, successfully
walked x64 stacks into its Hot/wmain functions and checked matching PDB module
information using ordinary privileges. Creation-time job association contained
the probe; no global tracing session or unrelated process was sampled.

## Decision 1: Reuse the workload and truthful policy controls

Build the existing selected throughput and read-diagnostic harnesses on the
admitted native backend. Preserve records, seeds, key/value streams, warm-up,
validation, fixed-work mutations, repetitions and benchmark/completion shapes.
Restore noinline intent with a compiler-supported private macro: MSVC uses
__declspec(noinline), while existing GNU/Clang attributes remain unchanged.

Modern Windows uses explicit weak namespace consent and its default mapped
path. Expose `copied`, not a falsely named Windows pread syscall, as the native
copied control. Preserve existing POSIX default/pread controls and semantics.
The pinned Windows pre-initialization mmap-limit helper is private. As in the
existing POSIX control, expose only its declaration in an authenticated
build-owned archive and record the control-patch digest; do not change the
reference algorithm or fabricate its friend test class to bypass access.
External source overrides remain untouched and advertise that copied control
as unavailable. Unavailable controls fail rather than silently selecting a
different policy. The declaration visibility patch does not change the
default mapped policy, but its digest remains recorded in those artifacts.
Use a separate unpatched build when an unmodified reference artifact is needed.

Use existing explicit reusable/owning Get and copying/exclusive batch controls.
For write-parity comparisons, select the existing `leveldb` WAL-creation
control. Native durable creation records file flushing plus weak namespace,
not POSIX file-and-directory persistence. Neither mode relaxes consent.
Compression, cache, buffer, checksum, access and ownership policies accompany
the result so backend and public-output costs are not conflated.

Native reference workloads use validated writable ASCII paths, since the
reference Env and Google Benchmark's narrow file options have that boundary.
Require an ASCII output/work root for this slice; do not implicitly depend on
the process ANSI code page or claim Unicode profiling argv support. Native
database/diagnostic Unicode capabilities remain unchanged.

## Decision 2: Collect only the newly launched owned workload

Add a private native collector executable. It launches the trusted benchmark
snapshot itself through CreateProcessW, with explicit application identity,
checked quoting, noninheritable defaults, selective inherited handles and
creation-time PROC_THREAD_ATTRIBUTE_JOB_LIST containment. A kill-on-close job
owns the process and descendants before its first thread resumes.

Do not provide attach-by-PID, enumerate data from other processes, start/stop
global ETW/WPR sessions, require administrator access, use POSIX ps/pgrep or
terminate by executable name. Toolhelp thread metadata is filtered immediately
to the owned PID; only those threads are opened or sampled. Log output is
redirected to an owned file, not an unbounded child pipe.
Snapshot filtering is not sufficient ownership evidence: a TID can be reused
before OpenThread. After opening, require GetProcessIdOfThread(handle) to
match the still-live owned child before timing/context queries or suspension.
Retain that validated handle through resume; close mismatches without sampling.

One wall deadline bounds readiness, capture, exit and artifact completion;
cleanup has a separate finite budget. Timeout or failure resumes every thread
the collector suspended, terminates only its job, waits/reaps and closes
handles. Capture cannot succeed while the child or inherited descendants
still survive. Parent death closes the job and cannot leave a suspended orphan.
Report the original failure plus explicit cleanup failure, never missing
observations as success. Reuse existing native quoting/containment prior art
without introducing a public process API or generic supervisor framework.

## Decision 3: Distinguish measured epochs from setup and calibration gaps

The collector creates an unnamed private shared control mapping and readiness/
proceed events. Only the exact owned child inherits these handles. Validate a
versioned control header and owned PID before using them; handles are internal
launch protocol, not ordinary user-facing profiling knobs.

The first ProfileInterval signals readiness after fixture creation/warm-up and
waits for the collector to initialize symbols before beginning measurement.
This protocol stays outside the timed workload. Subsequent invocations do not
repeat that wait. Keep macOS signpost behavior unchanged.

Use aligned Windows Interlocked operations on shared state, not an unsupported
assumption that arbitrary C++ atomics synchronize different processes. Each
invocation increments a monotonically increasing epoch and marks active/end.
The collector checks epoch and active state before and after sampling.
Thread-time baselines are keyed by thread identity, creation time and epoch;
never carry preparation CPU or calibration gaps into a new measured epoch.
A reused thread ID must not inherit a previous thread's CPU baseline.
Record observed epochs and completion evidence; no absent marker becomes an
empty successful profile.
The child also emits a separate versioned epoch ledger containing every
invocation's ID, completed iterations, start/end clocks and process CPU totals.
Bind its invocation count and final measured iteration count to the existing
completion/raw benchmark quantities. Preserve existing completion schemas.
Report short/missed calibration epochs and gaps explicitly; one sampled epoch
cannot stand in for all repetitions. A complete capture must cover the final
reported measured epoch, with validated CPU/wall/symbol coverage; otherwise
its outcome is partial or failed rather than complete.

## Decision 4: Report CPU-weighted stack sampling honestly

Sample with a declared high-resolution deterministic jitter schedule averaging
approximately 10 ms (initially 5, 7, 11, 13 and 17 ms). Use an owned
high-resolution waitable timer instead of scheduler-quantized Sleep. This
avoids locking every observation to one near-periodic workload phase while
remaining reproducible. Query owned-thread
CPU times, suspend briefly, capture CONTEXT, walk a bounded number of x64
frames with StackWalk64, and resume on every path. DbgHelp operations run on
one collector thread. Do not allocate or run helper code inside the child.
Known exited-thread races can be recorded/skipped after termination evidence;
other native failures remain failures.

Weight a captured stack by the positive CPU-time delta of that same thread
and epoch. Baseline-only/zero-delta observations are not CPU samples.
This is an approximation assigning an interval's thread CPU to sampled user
stacks, not ETW, exact kernel-stack attribution, wall-stack polling percentages
or an instruction-level CPU measurement. Expose method, rate, units,
truncation/unwind/symbol coverage and dropped observations.
Positive delta does not mean the thread is executing CPU work at observation:
a hot-then-wait loop can expose a wait stack after consuming CPU. Do not claim
idle exclusion from zero-delta filtering. For own-code attribution require a
benchmark-image leaf; native/system-leaf intervals retain their raw stacks but
their CPU remains explicitly unattributed. Report wall-stack observations
separately, never label their frequency as CPU percentages.

Before declaring the method usable, run optimized pure-hot, pure-wait and
alternating hot/wait calibration at the configured sampling interval, including
adversarial near-periodic phases. Reconcile observed and unattributed interval
CPU with the child's epoch ledger and publish those coverage fractions.
Choose and review acceptance bounds from that evidence; insufficient or aliased
final-epoch coverage produces a partial/non-success result. Recording a symbol
or positive delta alone is not calibration or full-profile admission.
The initial evidence-backed gate requires at least three own-leaf observations,
10% own-code attributed sampled CPU, and sampled CPU covering at least 5% of
the final epoch's child process CPU. Calibration additionally requires at
least 50% own attribution for pure hot work, 10% for both alternating phases,
and wait CPU no more than 20% of pure-hot CPU under equal duration.

Recording perturbs the workload; capture timings are never throughput or
speedup evidence. Run ordinary benchmarks separately for performance decisions.
Require positive measured CPU samples, a valid measured epoch, successful
workload/completion checks and meaningful owned-benchmark frames before
declaring capture complete.

## Decision 5: Bind symbols and snapshots without leaking identity

Compile optimized native profiling targets with private MSVC debug information
and matching link PDBs, without changing consumer/default library policies.
Resolve executable/PDB/collector paths through CMake target metadata; do not
guess .exe suffixes or accept an unrelated PDB.

The trusted runner snapshots and hashes benchmark binary, matching PDB and
collector before launch, verifies unchanged source/artifacts and records the
case/arguments/build/backend/reference/access policies. DbgHelp searches only
the owned snapshot directory; do not consult a network symbol server or leak
application data to external services.
Check loaded PDB identity/match information, not merely a nonempty symbol name.
Unsupported DbgHelp/SDK combinations or absent/mismatched own symbols produce
explicit non-success outcomes, never empty graphs advertised as profiles.

Raw stack/report output contains module basenames/offsets, functions, epoch and
owned-thread evidence, not developer-home/source-file paths or unrelated
process identities. Local build context retains existing explicit provenance;
repository docs/PR bodies contain no machine paths or personal attribution.
System modules may have partial symbols; report that coverage separately.

Use a versioned native stack-report schema and bind its raw samples, summary,
benchmark/completion and executable/PDB/collector digests in the run manifest.
Only successful validated captures publish a complete summary. Failed runs
retain explicit diagnostic status/artifacts rather than fabricating success.

## Build, tests and acceptance

Add a native Release profiling preset with supported MSVC flags and symbols,
not inherited GCC profiler/sanitizer flags. Separate normal benchmark runs,
instrumented read diagnostics and owned CPU capture tests. Preserve private
Google Benchmark/reference/codec isolation and disabled-feature consumers.
Keep all POSIX profiling/parser/collector contracts and gates unchanged.

Observe Windows selected-harness admission fail before enabling it, and
observe CPU-capture/epoch/symbol failure contracts fail before implementation.
Required evidence includes:

- all existing selected workloads/operation streams/completion counts on
  native Windows, in both engines and supported access/ownership/WAL controls;
- diagnostic mapped/copied setup reasons, borrowed-block and result totals,
  with no instrumentation in ordinary throughput results;
- a real optimized owned-child capture with positive CPU deltas, timed epochs,
  matching PDBs and useful own-function attribution;
- setup/zero-delta/calibration-gap exclusion, explicit unattributed CPU for
  hot/wait observations, and measured optimized calibration coverage;
- epoch ledger/repetition/iteration reconciliation, missed short epochs and
  final-epoch coverage rather than accepting another sampled interval;
- stale-TID/foreign-handle ownership revalidation without suspension/query,
  thread creation-time reuse and known exited-thread races;
- malformed control, wrong child, invalid launch, missing/mismatched symbols,
  stalled child, unwinding limitations and subprocess failures, all explicit;
- finite deadlines, resume-after-sampling, parent-death job cleanup, selective
  inheritance, no leaked handles/threads or interference with another process;
- artifact mutation and snapshot identity checks, nonempty report/summary
  validation, and collector timings never accepted as speedup evidence;
- comparative native mapped/reference selected-workload measurements and
  attribution of residual differences before proposing algorithm changes.

Independent measurement/storage and Windows/process/symbol code personas
review exact implementation and focused fixes before a green sequential merge.
Mapping/reference alignment is already delivered; profiling is evidence for
remaining differences, not permission to preemptively invent a new I/O policy.

## Design review record

Independent measurement/storage and Windows/process/symbol personas reviewed
the design. Four actionable gaps were accepted: positive CPU deltas can expose
wait stacks, polling can miss short epochs, mapped-read parity is a real
prerequisite, and a stale Toolhelp TID can become a foreign handle before open.

The corrected contract makes non-own-leaf interval CPU explicitly unattributed,
requires optimized adversarial calibration and coverage admission, binds a
complete child epoch ledger to final benchmark/completion quantities, records
missed calibration rather than claiming full coverage, and revalidates opened
thread ownership before queries or suspension. The DAG now includes mapping.
Focused follow-ups by both personas reported no remaining high-confidence
blocker. Implementation still must collect calibration/reconciliation evidence;
design clearance is not proof that capture works or measurements are accurate.

## Implementation evidence and code review

The admitted MSVC Release harness executes all existing read and fixed-work
write/mixed cases for Modern and the pinned reference. Native access controls
are named `default` and `copied`; external reference overrides remain
unmodified and reject their unavailable private copied control. An
authenticated build-owned archive exposes only that helper declaration and
records the patch digest. Direct proof exercised the copied control while the
owner-provided original checkout remained clean at the pinned revision.

The collector atomically job-contains its owned child before first execution,
inherits only log/control handles, validates opened-thread ownership, and
reconciles every callback/final iteration with an atomic child epoch ledger.
It hashes benchmark, benchmark PDB and collector snapshots, validates the
loaded benchmark PDB, publishes schema-2 module-relative stacks without
absolute addresses, derives all CPU/frame/epoch totals during validation, and
binds benchmark/completion/profile/ledger bytes in the manifest.

The final native performance selection contains 12 nonempty tests. It passes
41 shared report/runner contracts and five real collector contracts in
addition to all selected read workloads, fixed-work mutation streams,
mapped/copied diagnostics, calibration, CMake capabilities and dependency
isolation. The authenticated archive build also passes the forced reference
copied-control contract.
The final integration rerun also passes 718 native Debug cases, 714 Release
cases, and all 18 extended model/compatibility/process-recovery cases.

Initial fixed-period sampling was rejected by evidence: a 9 ms hot/1 ms wait
calibration phase received positive CPU deltas on wait leaves. An owned
high-resolution timer with the deterministic 5/7/11/13/17 ms schedule then
passed optimized pure-hot, pure-wait, alternating and phase-offset runs under
the reviewed bounds. Final validation also rejects truncated walks, unresolved
or wrong-module attributed leaves, impossible aggregate counters, per-epoch
CPU overruns, sparse final coverage, stale/foreign threads, mismatched symbols,
artifact mutation, readiness timeout and missing cleanup.

Same-machine selected-workload results at 65,536 records compared Modern with
the pinned Windows reference under recorded equivalent policies:

| Workload | Modern/reference wall ratio |
|---|---:|
| readrandom | 1.116x |
| readmissing | 1.127x |
| scan | 0.898x |
| retained seek | 1.237x |
| overwrite | 0.610x |
| writebatch, public copying | 0.614x |
| writebatch, exclusive control | 0.635x |
| writesync | 0.991x |
| mixed 50/50 | 0.833x |

Read profiles showed Modern process CPU no greater than the reference, with
the expected block decode/comparator/cache paths. Modern writebatch used
substantially less sampled CPU. Sync-write stack capture correctly produced
non-success because file-flush waiting had insufficient own CPU attribution;
its separate ordinary timing remained near parity. These local observations
show no evidence for another speculative read/write algorithm change. They
are not a universal performance SLA or production-readiness claim.

Independent measurement/storage and Win32/process/symbol code personas found
and closed impossible aggregate acceptance, unbound raw artifacts, calibration
coverage, duplicate/absolute unwind output, exited-thread races, unverified
collector-PDB claims and benchmark-module attribution. Focused closure and
runtime validation are recorded separately; no threshold, workload, persistent
format, namespace guarantee or POSIX collection contract was weakened.

## References

- [Sequential review](0006-sequential-pull-request-workflow.md)
- [Native process ownership](0065-windows-recovery-and-compatibility-verification.md)
- [Ordinary native baseline](0067-windows-comparative-benchmark-baseline.md)
- [Native mapped-read parity](0068-windows-mapped-read-parity.md)
- [Selected workload source](../../benchmarks/profiling_bench.cc)
- [Existing runner](../../tools/run_performance.py)
- [GetThreadTimes](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-getthreadtimes)
- [GetThreadContext](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-getthreadcontext)
- [StackWalk64](https://learn.microsoft.com/en-us/windows/win32/api/dbghelp/nf-dbghelp-stackwalk64)
- [SymGetModuleInfo64](https://learn.microsoft.com/en-us/windows/win32/api/dbghelp/nf-dbghelp-symgetmoduleinfo64)
- [Thread32First](https://learn.microsoft.com/en-us/windows/win32/api/tlhelp32/nf-tlhelp32-thread32first)
- [GetProcessIdOfThread](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-getprocessidofthread)
