# Windows path preparation and regular-file acquisition

## Scope and sequencing

Design for N01/N02/N05 from the
[production composition plan](production-function-composition-plan.md).
Change only windows_file_system.cc, its directly related native tests and this
document. No POSIX transaction, database volume admission, mapping, enumeration,
read completion, write buffering, public interface or fault-seam expansion.
Those remain independent recursive frontiers.

This batch is independent of engine/block implementation paths in #207/#208.
Do not modify their branches or the shared ledger while #207 owns it.
Accept and merge independently reviewed design before characterization and
production extraction. Keep path preparation and shared opening as separate
implementation topics within one reviewed PR.

## Path preparation

NativePath keeps its borrowed path/native string, empty/null check, device
namespace rejection and recognized extended namespace bypass in original order.
Accepted extended drive/UNC paths still return the supplied path without ordinary
DOS component validation or absolute resolution. Do not normalize literal
extended suffixes or make accepted namespace spellings case-insensitive.

`ValidateOrdinaryComponents(path)` performs the current component walk:
skip empty/dot/dot-dot/root-separator components, reject trailing dot/space first,
then invoke ReservedDosComponent. Preserve error codes and exact messages.
ReservedDosComponent remains unchanged in this batch; its stem/case/name
responsibilities still require recursive review.

`ResolveAbsolutePath(path)` owns the original GetFullPathNameW size query,
wchar buffer and grow/retry loop, returning Result<wstring> on success.
It calls FileError with the original path and immediately captured last error at
the same failing query sites. Do not add a retry bound, environment snapshot,
alternate path parser or injectable native API merely for this extraction.
The buffer still grows to the reported size and success remains size < capacity.

`EncodeExtendedPath(absolute)` consumes the resolved wide string and applies
the existing two output grammars: leading double separator becomes the explicit
extended UNC prefix plus suffix; otherwise prepend the extended drive prefix.
No new canonicalization, post-resolution validation or filesystem query.
NativePath composes the ordinary validation, resolution and output encoding.
New mutable Status/Result envelopes move their errors; do not add copies to
original direct path-error construction.

## Shared regular-file acquisition

Reuse existing OpenHandle, CheckKind and ScopedHandle rather than introduce
another Win32 opening stack or a generic resource framework.

`OpenRegularFile(path, access, share, disposition, flags, operations,
lock_contention = false)` prepares the native path, opens one configured handle
and validates regular disk-file kind. Keep the three original error-copy sites:
native.error(), handle.error() and the const CheckKind Status error.
Its new callers move the helper's error envelope rather than copying it again.
Success returns the ScopedHandle owner by implicit move; moved-from owners never
close the handle. A failed CheckKind destroys the acquired owner exactly once
before returning to the caller.

Preserve distinct native/path attribution: OpenHandle errors mention the prepared
extended path; CheckKind and append/lock errors retain the original supplied path.
NativePath's temporary wide/path storage can end at helper return because Open
consumes it synchronously and ScopedHandle borrows none of it. File objects still
copy the original path and retain the operations owner.

| Caller | Access | Sharing | Disposition | Flags | Later caller operation |
|---|---|---|---|---|---|
| OpenWriter truncate | GENERIC_WRITE | 0 | CREATE_ALWAYS | FILE_ATTRIBUTE_NORMAL | Wrap writable owner. |
| OpenWriter append | GENERIC_WRITE | 0 | OPEN_ALWAYS | FILE_ATTRIBUTE_NORMAL | Existing FILE_END positioning, then wrap. |
| OpenSequential | GENERIC_READ | READ and DELETE | OPEN_EXISTING | FILE_ATTRIBUTE_NORMAL | Wrap sequential owner. |
| OpenRandomAccess | GENERIC_READ | READ and DELETE | OPEN_EXISTING | FILE_FLAG_OVERLAPPED | Original copied/mapping policy and acquisition. |
| LockFile | READ and WRITE | READ and WRITE | OPEN_ALWAYS | FILE_ATTRIBUTE_NORMAL | Existing nonblocking whole-file lock and Busy translation. |

Keep lock_contention true only for LockFile's open. Do not extend Busy translation
to read/write sharing failures. CheckKind uses the original path and false
directory argument. Do not route directories, FileSize or namespace metadata
operations through the regular-file helper.

Append positioning and whole-file locking remain in their callers. Their
independent naming is a later recursive frontier, not a prerequisite for safe
sharing of acquisition. Native writable constructors may throw as originally;
the transferred scoped owner still releases the handle on constructor failure.
Do not insert explicit Close, catches, fallback opens or extra kind queries.

## Definition responsibility ledger

| Definition | Responsibility or composition boundary |
|---|---|
| ValidateOrdinaryComponents | Ordered ordinary DOS component admission using existing name predicate. |
| ResolveAbsolutePath | One native grow/retry resolution transaction and its exact error attribution. |
| EncodeExtendedPath | Two supported resolved-path representation grammars. |
| NativePath | Namespace admission followed by ordinary preparation composition. |
| OpenRegularFile | Native preparation, configured acquisition and checked owner transfer. |
| OpenWriter | Shared regular acquisition followed by existing append/writable ownership policy. |
| OpenSequential | Shared acquisition and sequential-object construction. |
| OpenRandomAccess | Shared acquisition before unchanged copied/mapped ownership policy. |
| LockFile | Shared acquisition before unchanged lock transaction. |

The unchanged predicates, FileError formatting, OpenHandle error classification,
CheckKind queries, object constructors and mapping/lock/append steps remain
recursive-review frontiers. This nine-definition batch is not certification of
the Windows file-system implementation.

## Characterization and delivery

Existing native cases cover invalid paths, Unicode/long/literal extended names,
sharing and kernel lock contention, append offsets, acquisition failures,
handle lifetime, mapping resources and weak namespace admission.

Before changing production, add original-source cases using existing FaultOperations:
record scalar access/share/disposition/flags and counts for the five opening modes;
assert one open/inspect on each success and the existing extended prefix.
Use mapping-disabled random reads to isolate acquisition, without changing the
mapping default or fault interface.

Add path admission priority across opening modes with fail_open enabled:
empty/null, device/unsupported extended namespace, ordinary reserved and trailing
suffix paths fail with original exact codes/messages before any native open.
Existing extended literal suffix cases demonstrate the bypass still works.
Add acquisition failure matrix across all five modes: open failure does not
inspect/close; inspect failure closes the acquired handle once and retains its
original-path native error; cleanup failure must not replace the primary error.
Successful objects retain the original operations owner and release handles.
Existing sharing-open Busy and kernel-lock cases remain required.

Rebuild the actual unit and Windows lock-child targets; run WindowsFileSystemTest,
WindowsFileSystemDeathTest and directly coupled native table/recovery tests using
their actual registered suite names. Require identical unique original/final
JUnit sets, required new cases and no failures/errors/skips. No threshold lowering
or new coverage exclusions. Native resolution resize failure cannot be
deterministically injected with the existing seam: state that limit rather than
claim an unexecuted oracle or add production seams to make a green test.
Independent combined review follows local verification; exact reviewed-head CI
must succeed before normal merge. Linux/macOS builds still verify no cross-platform
assembly changes, though they do not execute this Windows-only production file.

Independent bounded design review found no significant issue in namespace
priority, resolution, error/owner transfer, opening policy or characterization
feasibility. It did not edit files or execute builds/tests. Characterization and
extraction coverage remain unverified at this design-only stage, including the
explicit resolution fault-injection limit above.

## Implementation evidence

Accepted design #209 merged after all 20 exact-head checks succeeded.
Three new characterization cases passed on original production at
903252654898ffdc512ce49788d45e4e76708966 before extraction:
the five opening modes retain exact access/sharing/disposition/flags, one
open/inspection and eventual close; path syntax/namespace priority precedes
native acquisition; open/inspection failures retain primary errors and close
an acquired handle once even when cleanup reports failure.
Error attribution uses the existing PathUtf8 generic UTF-8 spelling rather than
the platform-native path separator. Existing extended literal suffix, Unicode,
sharing/kernel-lock, native mapping and owner-lifetime cases remain selected.

Path preparation and shared acquisition were separate production topic commits
after characterization. The nine-definition ledger is implemented without
public interfaces, fault-seam expansion, mapping/volume policies, new catches,
coverage exclusions or generic opening frameworks.
Exact rebuilt original/final Windows native/death/table/recovery JUnit sets
contain 121 unique passing cases, with all three required new cases and no
failures/errors/skips. Path-only intermediate selection passed 46 cases.
Formatting, documentation and whitespace gates apply before publication.
Resolution query/resize faults still have no deterministic existing-seam oracle.

Independent combined review found no significant issue in the three changed
paths and independently confirmed characterization-before-production commit
order, identical 121-case original/final artifacts, all three required cases
and the 46-case intermediate artifact. It did not edit files or execute
builds/tests; resolution fault branches, build provenance and CI remain outside
that static review's evidence.
