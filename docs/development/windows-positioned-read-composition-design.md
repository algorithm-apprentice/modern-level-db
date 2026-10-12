# Windows Positioned Read Composition

## Status and scope

Design proposal for the remaining Windows N09 frontier in the production
function composition plan. The inspected baseline is
61198b569993d63743e29c792f099c8aeed960c5.
Mapping/enumeration #218 must merge before characterization: reuse its existing
test-local recording seam instead of developing a competing same-file version.
This design-only document is independent of that implementation head.

Compose copied positioned reads and share their existing bounded request/range
decision with mapped Read. Keep TryReadView, sequential reads, mapping,
enumeration, writer progress and POSIX behavior outside this slice.
Implementation may change only the Windows production `.cc`, its existing unit
test file and this document. No public/private-seam header change, new fault
interface, benchmark, assertion or coverage exclusion is proposed.

## Contract before decomposition

Both copied and mapped Read first return zero for empty output, even at an
otherwise invalid offset. For nonempty output, clamp the requested size through
WindowsIoRequestSize, then reject an offset/request beyond signed 64-bit range.
Preserve the exact invalid-range error text and subtraction/short-circuit order.
Mapped reads validate this range before their mapped-length EOF check.

Copied Read owns one ScopedHandle event and one zero-initialized OVERLAPPED.
Encode low/high offset and the event in that operation; issue Read with a null
immediate-count pointer. Synchronous native success and ERROR_IO_PENDING both
proceed to Complete; immediate ERROR_HANDLE_EOF returns zero without Complete.
Other initial errors retain the positioned-read operation/path/code.

Complete success returns its byte count. Complete failure captures LastError
before any retirement, then retires a still-pending operation before translating
the captured code. ERROR_HANDLE_EOF still means zero; all other failures retain
the original complete-positioned-read error. Event-close failures during scoped
cleanup do not replace a read's primary error.

Kernel retirement is one coupled resource-safety operation:
check completion, request cancellation for exactly this OVERLAPPED if still
pending, and continue the existing blocking GetOverlappedResult loop until the
completion predicate is terminal. Preserve the original native calls and ignored
cleanup return values; do not add retry limits, early return on cancellation
failure, status defaults, alternate wait APIs or cancellation of unrelated I/O.
The captured primary error must survive all native cleanup calls.

The parent keeps file, event, OVERLAPPED and caller output alive through every
completion/retirement call. A helper borrows the parent's OVERLAPPED; it must
never create a local replacement, return while the kernel can reference it, or
let borrowed pointers escape.

Microsoft's API contracts explain this existing boundary: requesting cancellation
does not wait for completion and does not permit reusing/freeing OVERLAPPED;
GetOverlappedResult with a pending Internal state and bWait=TRUE waits for
completion. HasOverlappedIoCompleted determines terminal state, not whether the
operation succeeded. These references justify retaining the lifetime protocol,
not changing its policy in a refactoring PR.

## Recursive responsibility ledger

| Definition | Composition or domain leaf stopping reason |
|---|---|
| WindowsRandomAccessFile::Read | Empty-output gate, validated request, scoped event, operation encoding, initiation decision and completion; owns event/OVERLAPPED throughout children. |
| WindowsMappedRandomAccessFile::Read | Same validated request, mapped-length EOF, bounded copy; no native event/Read/Complete. |
| PositionedReadRequest | One bounded-request proof: clamp through WindowsIoRequestSize, check signed offset plus request, return DWORD or existing InvalidArgument. Shared only by the two existing range policies. |
| OpenReadEvent | One scoped native event acquisition and existing invalid-handle/error translation. |
| PositionedReadOperation | One coupled OVERLAPPED encoding: zero initialization plus low/high offset and parent event; returned by value before native I/O starts. |
| WindowsRandomAccessFile::BeginRead | One checked issuance decision: success/pending requires completion, immediate EOF does not, other initial failures retain their error. Return Result<bool> with that explicit meaning, not a generic callback/state framework. |
| WindowsRandomAccessFile::CompleteRead | One completion decision: observe native count/error, retire on failure, then translate the captured primary error or publish the count. |
| RetirePendingRead | One indivisible kernel-reference retirement protocol; cancellation and wait are coupled by the completion predicate. Borrow HANDLE/OVERLAPPED and remain noexcept. |

Use existing ScopedHandle, FileError and WindowsIoRequestSize.
Private methods borrow the file/path/operations already owned by their class;
file-local encoding/range/retirement helpers receive only their concrete inputs.
The boolean initiation result means exactly "Complete must run"; an ordinary
named local and explicit branch must make that meaning visible.
New propagation envelopes move existing Error values; do not introduce
additional copies. Do not place Status/Result in boolean chains or conditional
expressions, and do not add wrappers around individual standard/native calls.

## Original-source characterization

After #218 is delivered, inspect its actual merged recording seam and extend
only test-local observation/configuration. Keep fixed-capacity recording and
avoid allocation inside new noexcept native overrides.

Required original-production cases:

1. Copied/mapped empty and signed-range boundaries. Empty output at UINT64_MAX
   returns zero without native calls. A valid one-byte request at INT64_MAX-1
   has the original EOF behavior; a two-byte request at that same offset
   and a UINT64_MAX nonempty offset report the exact range error before
   creating an event or performing native I/O. Assert both owner categories.
2. Copied initiation/completion trace matrix: immediate success, forced pending,
   immediate EOF, initial error, event error, completed EOF and completed error.
   Record request, low/high offset, nonnull per-call event, null immediate-count
   pointer and exact Event/Read/Complete/Close prefixes. Validate actual bytes and
   close-event identity/liveness, plus primary error over event-close failure.
3. Per-call operation ownership/reuse: repeated positioned reads use the correct
   offsets and retain no kernel references after the call. Completed stack
   storage and closed native handle values may be reused; do not assert address
   or handle-value uniqueness across calls. Record operation field snapshots,
   never dereference a retained stack pointer after return. Existing native
   concurrent reads and owner-outlives-filesystem cases remain selected.

Use real files and valid caller-owned buffers. Do not create oversized spans
over small arrays merely to test DWORD clamping; retain direct native request
clamp tests or add a pure-helper case only when a real public-path buffer exists.

The existing fail_complete seam calls the real completion before reporting its
injected failure. It does not establish the still-kernel-pending cancel/drain
path. Document that limit explicitly. Do not forge OVERLAPPED.Internal or use a
fake pending operation with real CancelIoEx/GetOverlappedResult: that can hang or
exercise an invalid kernel contract. Real-kernel cancellation and allocation
throws remain inspection-based in this slice; deterministic seam expansion is
not justified solely to move unchanged code.

## Small topics and delivery

Independently review/accept the design before source edits. Characterize original
production after #218 merges, recording exact native JUnit case names.
Implement shared request proof/operation preparation as one responsibility topic,
then copied issuance/completion/retirement as another. Validate targeted native
read cases between them and compare the combined original/final selected sets.

Retain the native Windows filesystem/death, table, recovery and database cases
used by the preceding slice, plus all new required read cases. Require exact
unique classname/name set equality, no failures/errors/skips, existing formatting
and documentation checks, and independent combined implementation review before
publication. All exact reviewed-head hosted CI checks must pass before normal
merge; Linux/GCC coverage does not by itself execute this Windows-only protocol.
Verify reviewed changed paths persist after merge. This slice does not complete
the production-wide recursive audit or certify unchanged child functions.

## Authoritative API references

Microsoft Learn API documentation consulted on October 12, 2026:

- CancelIoEx, especially completion versus cancellation and OVERLAPPED lifetime:
  `https://learn.microsoft.com/en-us/windows/win32/api/ioapiset/nf-ioapiset-cancelioex`
- GetOverlappedResult, especially pending Internal state, bWait and per-call event:
  `https://learn.microsoft.com/en-us/windows/win32/api/ioapiset/nf-ioapiset-getoverlappedresult`
- ReadFile, especially overlapped request/buffer ownership and pending completion:
  `https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-readfile`
- HasOverlappedIoCompleted, completion-state predicate versus operation outcome:
  `https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-hasoverlappediocompleted`

## Independent design review

Bounded read-only review found no significant issue in the proposed ledger,
range boundaries, parent-owned operation lifetime, primary-error preservation
and retained cancellation/draining protocol. It confirmed the explicit boolean
initiation meaning and feasible original-source characterization limits.
Source/tests remain unchanged; the reviewer did not edit files or execute
builds/tests. Genuine pending cancellation and allocation throws remain
inspection-based, and characterization still waits for #218 delivery.

## Original characterization stage

Design #220 is accepted; #218 delivered as
3e9d2310508c795255de5ded54d813bedebbfbd3 before this stage begins.
Three new public-read cases cover copied/mapped empty/range ordering, seven
issuance/completion/error/EOF prefixes and primary errors over event-close
failure, and repeated low/high offset and per-call operation snapshots.

The test-local recorder adds Event/Read/Complete observations, configurable
native error codes and value snapshots only; it retains no OVERLAPPED pointer.
The large valid-offset range case injects immediate native EOF rather than
assuming every real filesystem supports a read near INT64_MAX. Existing real
sparse-offset, EOF and concurrent cases remain selected.
Completion observations occur after the real wait and before injected failure;
they do not claim to execute genuine still-pending cancellation.

Native Windows Debug on unchanged production passes 166 unique cases across
WindowsFileSystemTest, WindowsFileSystemDeathTest, TableTest, RecoveryTest and
DatabaseTest, including all three new cases. Their JUnit case set is retained
for exact implementation comparison. No source or production seam is changed
at this stage; final implementation execution remains pending.

Independent bounded characterization review found no significant issue,
verified the original 166-case XML and unchanged production/private header,
and confirmed that the new snapshots do not retain pointers or fabricate
pending kernel state. It did not execute builds/tests. Real synchronous versus
pending outcomes remain environment-dependent; genuine pending retirement
and allocation throws remain inspection-based.
