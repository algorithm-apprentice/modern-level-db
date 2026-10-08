# ADR-0065: Windows Recovery and Compatibility Verification

## Status and scope

Proposed on 2026-10-08 after database integration PR #99. This design adds
verification and build admission, not a stronger Windows durability guarantee.
Independent storage/failure-model and Windows/process/build reviews precede
implementation; owner-authorized sequential delivery still requires this PR
to be reviewed, green, and merged first.

The slice completes `verify-windows-compatibility-crash` from
[ADR-0064](0064-windows-filesystem-and-delivery.md). It enables the existing
model, golden, differential, and deterministic fault tiers on the admitted
native backend, and supplies Windows process-only crash tests.
Diagnostics, benchmarks, mappings, and CPU capture remain separate slices.

## Current evidence and prior art

The native filesystem and opted-in database now pass real MSVC Debug/Release
tests for file flushing, Unicode/long paths, native lock exclusion,
compaction/snapshot pins, and failed-recovery orphan filename reuse.
Those tests do not replace the existing extended harness.

The root CMake guard still admits that harness only on Linux/macOS.
Its model/golden/differential and in-memory power-loss tests are portable,
but `process_crash_test.cc` directly uses fork, _exit, and waitpid.
Modern's test clients must explicitly opt into Windows' weak namespace mode.
The private pinned LevelDB reference already has a Windows Env, but the
current reference helper restores RTTI only for GNU/Clang/AppleClang.

Adopt established mechanisms:

- Pinned LevelDB's Windows Env for the reference database and existing
  bidirectional cross-open/model traces; do not replace the authenticated
  reference target with a different implementation.
- GoogleTest v1.17.0 `gtest-death-test.cc` uses a dedicated Windows child
  process, anonymous-pipe observations, native process handles, and exit-code
  checking. Adopt process isolation and handle ownership, not its ANSI
  process launch or unbounded waits.
- Microsoft's redirected-child example establishes pipe-end ownership.
  Use CreateProcessW, explicit application identity, selective inherited
  handles, and checked Win32 results rather than a shell launcher.
- A kill-on-close Job Object contains the test-owned helper if the parent
  aborts. This is an OS lifetime primitive, not a new production executor.

No production subprocess API, VFS extension, new persistent format,
checksum/compression policy, or recovery algorithm is introduced.

## Decision 1: Admit actual portable tiers independently of process syscalls

When extended tests are requested, allow an admitted POSIX or native Windows
database backend. Keep unsupported Windows targets/toolchains rejected.
Do not remove the tools/benchmark/profiling capability guards as a side effect.
Keep `MODERN_LEVELDB_BUILD_TESTS` and `BUILD_TESTING` prerequisites and
nonempty CTest selections.

The extended aggregate target still contains:

| Tier | Windows implementation and evidence |
|---|---|
| model | Existing seeded binary keys, snapshots, scans, batches, and reopen |
| compatibility | Existing golden files, both codecs, and original-LevelDB differential/cross-open |
| deterministic crash | Existing CrashFileSystem/MemoryFileSystem ordering and torn-WAL cases |
| process crash | New native child tests; existing fork test remains POSIX-selected |

At Windows disk-fixture call sites, set
`Options::allow_weak_namespace_durability = true` explicitly, including the
golden reopen path. Do not change the public default or silently inject
consent into every library open. In-memory strong-protocol fault tests keep
their current injected filesystem semantics.

The local original LevelDB repository may be provided through the existing
FetchContent source override. Do not commit a developer-machine path. Preserve
the pinned fallback, private codec targets, and provenance. On MSVC, restore
RTTI for the reference target itself with a later target-local /GR option,
as existing non-MSVC restoration does. Upstream no-exception/RTTI flags must
not leak into Modern, test clients, or parent consumers. Retain CRT and
exception boundaries; this is not approval for arbitrary ABI workarounds.

Cross-engine fixtures use ASCII absolute paths because the pinned Windows
reference uses ANSI filesystem APIs. Modern-only native tests continue to
cover Unicode; a reference code-page limitation must not be mistaken for
Modern format incompatibility.

## Decision 2: Use one private owned-child fixture

Add Windows-only test support and a dedicated crash-child executable.
Neither becomes a public library header or production dependency. The child
contains no descendant-process launcher.

The parent owns:

- one anonymous pipe read end;
- the transient inheritable write end until launch completes;
- the native child process and primary-thread handles;
- an attribute-list lifetime used only during launch; and
- a kill-on-close Job Object containing that child.

All acquisitions immediately enter RAII ownership. Partial acquisition or
launch failure releases earlier resources. Child creation uses a native
absolute executable supplied by CMake's target identity, a mutable Unicode
command line, and no command interpreter or PATH search. Encode generated
executable names correctly even when the build directory contains Unicode.
Quote path arguments according to Windows argv rules; database paths are
native wide paths and never round-trip through an ANSI constructor.

Use `PROC_THREAD_ATTRIBUTE_HANDLE_LIST` so only the observation write handle
is inherited. Database, process, job, and read-pipe handles remain
noninheritable. Do not rely merely on bInheritHandles with the entire parent's
inheritable set. The parent closes its write end after a successful launch;
otherwise pipe EOF would not prove child completion.

Supply `PROC_THREAD_ATTRIBUTE_JOB_LIST` alongside the inherited-handle list
so the configured kill-on-close job is associated atomically during process
creation. A create-then-AssignProcessToJobObject sequence would leave an orphan
window if the parent died between those calls, even with a suspended child.
Create suspended and resume only after launch bookkeeping succeeds. If
creation/resume fails, report failure and terminate/reap any owned process;
never fall back to launching outside the job. Windows 10/11 job-list and
nested-job behavior are the admitted environment.

No PID/name-based process discovery or system-wide termination is needed.
Terminate only the owned process/job handles. The primary-thread handle is
released once resume succeeds. The process handle remains until a terminal
wait result and exit code are observed.

## Decision 3: Establish acknowledgements before inducing the crash

The helper supports fixed ASCII modes, not arbitrary commands:

| Mode | Action |
|---|---|
| exit | Write eight synced large records, emit observations, then ExitProcess without C++ database destruction |
| hold | Write the same records and hold the open database until parent-owned forced termination |
| malformed | Emit a deliberately invalid observation for parser failure validation |
| tail | Emit a valid acknowledgement/readiness prefix followed by invalid trailing output |
| stall | Emit no readiness and wait, allowing timeout/cleanup validation |

Both real-write modes open with explicit weak namespace consent and a 64 KiB
write buffer so writes exercise WAL rotation and background maintenance.
Each successful synchronous Put is followed by an unbuffered checked pipe
write:

```text
ack 0
ack 1
...
ack 7
ready
```

Every line is ASCII, newline-terminated, and sent only after the corresponding
Put has returned success. Handle successful short pipe writes and reject
zero progress. No user keys/values, paths, or localized error text enter
the observation protocol. On child failure emit a fixed failure record if
possible and return a distinct nonzero exit code; missing observations and
nonzero exit are failures, never a smaller successful evidence set.

The parent requires exactly ordered acknowledgements 0 through 7, one ready,
no duplicate/unknown lines, no unterminated trailing bytes, and bounded total
output. Pipe read chunk boundaries do not define protocol lines.
Reject malformed and oversized streams. EOF before readiness is a failure
even if the child exited zero.

After readiness:

- exit mode must terminate with the expected zero exit code;
- hold mode must still own the database lock, then be terminated through the
  owned handle and reach a terminal wait state; and
- only after process completion may the parent reopen the database and
  verify all eight acknowledged values exactly.

Readiness alone is not final protocol success. After the observed terminal
process state, drain and validate the pipe through EOF under a bounded
deadline, including buffered output after ready. The tail fixture must
demonstrate that an otherwise valid prefix cannot hide invalid trailing bytes.

Verify lock exclusion before forced termination and reacquisition afterward.
Use both ordinary and Unicode database paths. Do not inherit the parent's
database handles, which would make post-exit lock evidence misleading.

## Decision 4: Bound observation, termination, and cleanup

Use a steady-clock deadline for the whole readiness phase rather than
resetting it after each byte. PeekNamedPipe plus bounded reads avoids
blocking indefinitely on an anonymous pipe that has no data. Check process
state and pipe errors while waiting; do not treat EOF or Peek failure as ready.

Allow at most thirty seconds for all acknowledgements/readiness, ten seconds
for normal exit, and five seconds for forced termination/reaping. Protocol
streams have a small fixed size limit (4 KiB is sufficient for these records).
Outer CTest timeouts must exceed the complete per-test budget, including
multiple child runs and cleanup; use the extended tier's 180-second budget
or a larger explicitly justified value if one case composes more work.

Explicit completion methods report native failures with operation and numeric
code. An early assertion, exception, or timeout still triggers contained
child termination and bounded reaping. Destructors cannot return errors;
report cleanup failure to the test error stream rather than silently
asserting clean completion. A kill-on-close job remains the backstop if the
parent process itself dies before its destructors run.

The parser has dependency-free unit tests for split lines, ordered records,
duplicates, unknown/missing readiness, oversize output, and EOF fragments.
Real native fixtures separately verify launch, malformed child, timeout and
cleanup, expected exit, forced termination, and database lock recovery.
No polling scheduler, generic process registry, or collector is added.

## Failure model and nonclaims

These are **process-only** failures while Windows, NTFS, and the storage
device remain running. Surviving acknowledged synchronous records are the
required evidence. Unsynced records and an unacknowledged in-flight batch are
not required to survive; if later exercised, a batch may be wholly present
or absent, but not partially committed.

ExitProcess bypasses C++ database destructors; DLL detach and OS handle
cleanup still occur. Forced TerminateProcess is a separate case and should
not be described as the same shutdown path.

The in-memory power-loss model proves the engine's strong abstract ordering.
It does not make the explicit Windows weak-directory policy power-loss safe.
No OS crash, power-cut, drive-cache, mapping fault, or production-readiness
claim follows from passing these tests.

## Implementation/review/verification gate

Implement one verification PR after this design is reviewed and merged.
Observe requested Windows extended configuration fail under the current
POSIX-only guard before admitting it. Express acknowledgement/parser failure
behavior with failing tests before implementing the fixture.

Required exit evidence:

- Native MSVC Debug/Release build with warnings as errors and working private
  reference RTTI/codec/CRT linkage.
- Existing model and golden/differential cases pass, including both directions
  of database-directory cross-open and None/Snappy/Zstd.
- All deterministic fault/torn-WAL tests remain unchanged and passing.
- All process observations, terminal states, recovery values, and lock
  transitions are verified; malformed/timeout fixtures fail explicitly and
  leave no live owned child.
- Windows-compatible nonempty presets and CI select actual native extended
  cases; the existing POSIX process cases and sanitizer gates remain intact.
- Independent storage/failure-model and Win32/process/build personas review
  the exact implementation, assess concrete findings, and verify focused
  fixes before autonomous merge.

## Design review record

Independent storage/failure-evidence and Win32/process/build personas reviewed
the design. The process reviewer identified the create-before-job-assignment
parent-death gap and recommended explicit post-readiness EOF validation.
Both were accepted: creation-time job association is mandatory, and a
valid-prefix/invalid-tail fixture verifies final stream rejection.

A focused closure review confirmed Windows 10/11 API admission, ownership
ordering, bounded cleanup, and the corrected terminal/protocol requirements.
No actionable finding remains from those bounded design passes.
ASCII English, LF/whitespace, code fences, local links, DAG ordering, and the
exact documentation-only changed-file selection were checked. No production
build, child launch, or storage-runtime result is claimed by this design PR.

## References

- [ADR-0006 review gates](0006-sequential-pull-request-workflow.md)
- [ADR-0040 original harness](0040-compatibility-and-crash-harness.md)
- [ADR-0064 Windows contracts](0064-windows-filesystem-and-delivery.md)
- [Current process test](../../tests/crash/process_crash_test.cc)
- [GoogleTest v1.17.0 death-test implementation](https://github.com/google/googletest/blob/v1.17.0/googletest/src/gtest-death-test.cc)
- [Pinned LevelDB Windows Env](https://github.com/google/leveldb/blob/7ee830d02b623e8ffe0b95d59a74db1e58da04c5/util/env_windows.cc)
- [Redirected child-process pipe ownership](https://learn.microsoft.com/en-us/windows/win32/procthread/creating-a-child-process-with-redirected-input-and-output)
- [CreateProcessW](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-createprocessw)
- [Process attribute list](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-updateprocthreadattributelist)
- [Job Object lifetime](https://learn.microsoft.com/en-us/windows/win32/api/jobapi2/nf-jobapi2-setinformationjobobject)
- [Job association during creation](https://learn.microsoft.com/en-us/windows/win32/procthread/job-objects)
- [PeekNamedPipe](https://learn.microsoft.com/en-us/windows/win32/api/namedpipeapi/nf-namedpipeapi-peeknamedpipe)
