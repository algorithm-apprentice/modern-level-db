# Native Read Handle Lifecycle Completion

## Scope

Hardest-first completion of native sequential and copied-positioned read
ownership. Source baseline:
`45b46ca61848254ff19b1c18390efd2d2423f690`.

This batch audits 62 C++ definitions and no lambdas:

| Area | Definitions | Included responsibilities |
|---|---:|---|
| POSIX read lifecycle | 21 | `ScopedDescriptor`, native open/request/offset leaves, sequential and copied random readers, copied publication, `OpenSequential` and `OpenRandomAccess`. |
| Windows read lifecycle | 22 | `ScopedHandle`, sequential reader, positioned request/event/operation/retirement leaves, copied random reader, copied publication and filesystem open entrypoints. |
| Windows native seam | 19 | `WindowsIoRequestSize` plus all `WindowsFileOperations` destructor/native-call definitions. |

Mapped-region ownership is complete in #268. Windows positioned-read
completion/drain was selected by N09; this batch records its complete leaf
disposition and preserves it. No read algorithm, caching policy, request size or
performance behavior changes. Benchmarks remain out of scope.

## POSIX descriptor ownership

`ScopedDescriptor` is the native descriptor owner. A sequential or copied
random reader should store that owner directly rather than copy its integer and
require the opening function to call `Release`.

`OpenSequential` and copied fallback construct the reader from an rvalue
`ScopedDescriptor`. If allocation or path-parameter construction fails before
transfer, the caller's owner closes the descriptor. If reader member
initialization fails after transfer, the partially constructed owner closes it.
Successful publication moves exactly one owner into the reader, whose
destructor can be defaulted.

The reader constructors are `noexcept` after argument construction: descriptor,
path and owner initialization are moves with nonthrowing contracts. This keeps
raw-`new`/coverage behavior explicit without changing exceptions from copying
the path argument at the call site.

Sequential reads keep the exact empty-read fast path, bounded native request,
EINTR retry, short-read/EOF result and `"read"` error mapping. Positioned reads
first validate `off_t`, then keep the same request bound, EINTR retry,
short-read/EOF result and `"positioned read"` error mapping. These syscall loops
remain separate leaves because `read` advances shared stream state while
`pread` does not.

## Windows handle and positioned-read lifecycle

`ScopedHandle` already owns native handles and is moved directly into sequential
and copied random readers. Both reader constructors become explicitly
`noexcept`; no ownership structure changes.

Sequential `Read` is a leaf around one bounded `ReadFile` operation. Empty
output succeeds without a native call, `ERROR_HANDLE_EOF` is zero bytes, and
other failures retain `"read"` diagnostics.

Copied positioned `Read` remains this composition:

1. reject an unrepresentable signed 64-bit range before native calls;
2. create one manual-reset event owner;
3. initialize one zeroed `OVERLAPPED` snapshot;
4. begin the read;
5. complete or drain/cancel it while event, operation and output remain alive;
6. close the event only after the operation is terminal.

`BeginRead` distinguishes immediate EOF, immediate error, synchronous success
and `ERROR_IO_PENDING`. `CompleteRead` captures the primary completion error,
retires any still-pending operation before constructing the returned result,
then maps EOF or I/O. `RetirePendingRead` is a cleanup leaf: request cancellation
and keep waiting until the kernel no longer owns the stack `OVERLAPPED` or
caller buffer.

The file owner and retained `WindowsFileOperations` outlive the originating
filesystem. Per-read event cleanup never closes the file handle.

## Windows native seam

`WindowsIoRequestSize` is the one DWORD request clamp. Every
`WindowsFileOperations` definition is a thin `noexcept` native-call leaf used
for deterministic fault characterization. The seam owns no handles and applies
no error policy; callers capture `GetLastError` immediately and retain resource
ownership.

Do not merge unrelated native operations into a command enum or generic syscall
dispatcher. Their typed signatures teach the ownership and completion
contracts directly.

## Characterization and validation

Before production edits, preserve:

- six POSIX identities for sequential EOF, concurrent positioned reads, short
  reads/range rejection, copied fallback lifetime and descriptor release;
- ten Windows identities for sequential/copied reads, sparse offsets, pending
  completion, event/read failure cleanup, range guards, stage/error prefixes,
  repeated operation snapshots and copied-handle lifetime;
- one new POSIX characterization proving a sequential reader outlives its
  filesystem and an unlinked pathname.

Original POSIX identities come from the unchanged Hosted GCC 13 run for #268;
Windows identities are captured locally. The new POSIX test is committed before
descriptor transfer; the local WSL image lacks a C++ frontend, so the unchanged
kernel behavior is checked directly and the exact C++ identity remains a Hosted
GCC 13 requirement.

Original evidence is 6/6 Hosted POSIX identities and 10/10 local Windows Debug
identities. The additional POSIX ownership oracle is also checked directly
against the host kernel: an open reader survives unlink, reads the retained
bytes and returns the process descriptor count after close.

After each responsibility-sized production commit, run the affected native
selection. Final identities must exactly match the characterized set. Run
Windows Debug/Release, complete Windows Debug and Hosted GCC 13 coverage on the
reviewed PR head.

## Definition disposition

The final ledger retains 62 definitions and no lambdas. Descriptor/handle
special members are ownership leaves. Request clamps, offset conversion, native
open, event creation, operation initialization, begin, completion and retirement
are named leaves. Public read/open methods are compositions over those leaves.

No runtime assertion, diagnostic conditional compilation, read optimization or
cross-platform syscall abstraction is introduced.

## Final implementation ledger

All 62 original definitions remain:

| Area | Final definitions | Reviewed responsibility |
|---|---:|---|
| POSIX read lifecycle | 21 | Descriptor ownership/opening, request and offset leaves, sequential/copied readers and filesystem publication. |
| Windows read lifecycle | 22 | Handle ownership, sequential/positioned stages, copied reader and filesystem publication. |
| Windows native seam | 19 | DWORD request clamp and typed native fault-seam leaves. |

POSIX readers now store `ScopedDescriptor` directly and use defaulted
destructors. Their path member precedes the descriptor member so reverse member
destruction still closes the descriptor before destroying the retained path.
Copied and sequential publication move the owner instead of extracting an
integer and calling `Release`.

POSIX sequential/random and Windows sequential/random reader constructors make
their nonthrowing transfer initialization explicit. Exceptions from allocation
or by-value argument construction still occur before the constructor transfer
body and leave a live RAII owner to close the resource.

Local Windows Debug and Release both pass the exact 10 characterized identities.
Four concurrent/pending/stage snapshot identities pass 20 consecutive
executions, and the complete Windows Debug suite passes all 868 tests. Final
POSIX compilation and the seventh ownership identity remain Hosted GCC 13
requirements.

## Integrated review

Review verified allocation and parameter-initialization ordering, exactly-once
close after moved-from owners, and close-before-path member destruction. The
Windows N09 state machine and all native call prefixes remain byte-for-byte
unchanged apart from the constructor exception specification.

## Review focus

Review transfer timing around allocation and path construction, moved-from owner
inactivity, exactly-once descriptor close, empty/range guards before native
calls, EINTR retry scope, event close after terminal completion, primary-error
priority and retained fault-seam lifetime.
