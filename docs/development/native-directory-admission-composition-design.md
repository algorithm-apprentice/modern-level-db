# Windows database directory admission composition

## Scope

Design N03/N04 after native path and regular acquisition delivery in #212.
Change windows_file_system.cc, direct Windows native characterization and this
document only. No POSIX admission, public/fault interface, directory creation,
mapping, enumeration, kernel lock, writer/read policy or durability changes.
The engine iterator batch #207 and shared ledger remain independent.

PrepareDatabaseDirectory must continue to admit an existing directory or the
single missing final component, resolving aliases before storage policy.
It does not create directories or walk multiple missing ancestors.
Keep NativePath validation before weak-durability consent, consent before
initial DrivePath admission, and all three before attributes/open queries.

## Directory inspection and resolution

`TrimDirectorySeparators(native)` mutates the already owned prepared path using
the current parent_path loop while native length exceeds the extended drive
root and has_filename is false. Do not perform lexical normalization or change
root/UNC policy. This operation can allocate as originally; do not mark noexcept.

`DirectoryIsMissing(native, original)` performs the original GetFileAttributesW
query. Return Result<bool>: file/path-not-found means missing, other native
failure uses FileError with the original path, non-directory is the original
InvalidArgument, existing directory means false. A missing final component
does not mean the parent exists; OpenDirectory remains authoritative.

Keep OpenDirectory(missing ? native.parent_path() : native, operations) in the
parent so the ScopedHandle remains alive through resolution, policy, explicit
close and final-path construction. Preserve its original error copy.
Do not route directory handles through OpenRegularFile.

`ResolveDirectoryHandle(handle, original)` performs the existing
GetFinalPathNameByHandleW size query, required-plus-one allocation and second
query under VOLUME_NAME_DOS. Return Result<filesystem::path> from the resolved
wide bytes. Keep last-error capture for zero and explicit
ERROR_INSUFFICIENT_BUFFER for length >= capacity. Do not substitute the ordinary
path resolution loop, add a retry, or change native query count.
Its input handle is borrowed; the result owns its resolved path bytes.

## Resolved storage admission

`CheckDatabaseVolume(final, original, operations)` first checks DrivePath with
the original resolved-volume NotSupported message, then composes:

- `DatabaseVolumeRoot(final, original)` retains the max(final length + one,
  MAX_PATH + one) wchar buffer and GetVolumePathNameW query. Return the buffer
  owner so its terminated root survives both following native calls without
  another length scan or string copy.
- `CheckFixedVolume(volume, operations)` queries DriveType exactly once;
  UNKNOWN/NO_ROOT_DIR remain Io, all other non-FIXED kinds NotSupported.
- `CheckNtfsVolume(volume, original, operations)` retains the fixed 32-wchar
  output, VolumeInformation query and case-insensitive CompareStringOrdinal
  against NTFS. Native query errors retain FileError, unsupported filesystem
  remains the original NotSupported. Do not make a successful root/fixed-drive
  check an alternate success for filesystem failure.

CheckDatabaseVolume retains root-before-drive-before-filesystem ordering and
stops at the first failure. New mutable Result/Status error envelopes move;
original directly constructed errors and existing const Status copy sites stay.
No generic volume-policy configuration or cross-platform admission framework.

PrepareDatabaseDirectory composes guards, trim, missing inspection, acquired
directory resolution, volume admission, original explicit CloseHandle and final
existing/missing path publication. Const close failure retains its original
error copy. Return missing ? final / native.filename() : final only after close
success, without moving native earlier or changing the original path allocation
timing. A prior failure unwinds the directory owner and ignores cleanup failure
as originally; a successful policy followed by explicit-close failure is failure.
The shared operations owner and native handle lifetime are unchanged.

## Definition responsibility ledger

| Definition | Responsibility or composition boundary |
|---|---|
| TrimDirectorySeparators | One accepted directory-path tail representation adjustment. |
| DirectoryIsMissing | One attributes query and existing/missing/wrong-kind classification. |
| ResolveDirectoryHandle | One bounded handle-to-resolved-path query transaction. |
| DatabaseVolumeRoot | One resolved-directory-to-volume-root query and buffer ownership. |
| CheckFixedVolume | One native drive-type admission decision. |
| CheckNtfsVolume | One native filesystem-name admission transaction. |
| CheckDatabaseVolume | Resolved namespace, root, drive and filesystem composition. |
| PrepareDatabaseDirectory | Guards, directory transaction, storage admission, close and publication. |

NativePath children, directory OpenDirectory/CheckKind, FileError/CloseHandle,
mapping and other native operations remain recursive audit frontiers.
These eight definitions do not certify the entire Windows implementation.

## Original characterization and delivery

Existing tests cover consent, aliases/junction resolution, wrong directory kind,
missing directory behavior, query versus unsupported storage errors and cleanup.
Before extraction strengthen FaultOperations with test-local drive/volume query
counts and add three original-source characterization cases:

- Mixed guard priority across invalid syntax, absent consent and admitted
  extended UNC spelling. Assert exact error code/text and no open/inspection,
  drive/filesystem queries or close; wrong existing file kind also precedes
  injected directory-open/volume failure.
- Existing and single-missing-final directories, with trailing separators and
  junction alias controls. Assert the exact returned resolved path, directory
  open access/sharing/disposition/flags, one inspection/drive/filesystem/close
  and no creation. Retain original missing-parent failure without walking up.
- Failure priority matrix: directory-open/inspection, UNKNOWN/NO_ROOT_DIR,
  nonfixed drive, filesystem query, unsupported filesystem and explicit close.
  Combine later failures and cleanup failures, assert first code/message,
  only the appropriate query prefix, acquired handle released once and no path
  publication/creation. Reuse PathUtf8 for existing original-path native error
  spelling and assert codes instead of locale-dependent system message suffixes.

Native attribute/final-path/volume-root query faults lack deterministic existing
fault seams: acknowledge the coverage/oracle limit, do not expand production
interfaces or claim tests for unexecuted branches.
Run original cases before separate directory-resolution and volume-admission
topic commits. Rebuild actual unit and Windows lock-child targets and select
WindowsFileSystemTest, WindowsFileSystemDeathTest, RecoveryTest and DatabaseTest.
Require identical unique original/final JUnit sets, required new cases and no
failures/errors/skips, plus intermediate native-only evidence.
Formatting/docs/whitespace and existing coverage gates remain unchanged.
Independent design review precedes design merge; independent combined review
and successful reviewed exact-head CI precede normal implementation delivery.

Independent bounded design review found no significant issue in the eight
responsibilities, guard/query order, owned buffers, parent handle lifetime or
error-copy preservation. It did not edit files or execute builds/tests.
Missing-parent and preflight wrong-kind cases must still be added before source
extraction; native query faults/counts and allocation-exception ordering remain
inspection-based limits rather than claims of executed characterization.

## Implementation evidence

Accepted design #213 merged after all 20 exact-head checks succeeded.
Three new cases passed against original production at
9ef8613cbc67d792384c0477f2aa8e0f532d9c7c before extraction. They assert guard
priority without acquisition, existing/single-missing alias resolution with
trailing separators and exact directory opening policy, missing-parent rejection,
and eight first-failure/query-prefix/cleanup outcomes without directory creation.
Failure messages use original PathUtf8 attribution and stable native-code prefixes,
not localized system suffixes.

Characterization preceded separate directory inspection/resolution and resolved
volume-admission production topic commits. The eight-definition ledger is
implemented; the directory owner remains in the parent until explicit close,
and final existing/missing path construction still follows close success.
Exact rebuilt original/final native/death/recovery/database JUnit sets contain
120 unique passing cases, with all three required new cases and no
failures/errors/skips. Directory-resolution intermediate selection passed 49 cases.
No public/fault interfaces, query counts, directory creation, durability policy,
coverage exclusions or other platform operations changed.
Formatting, documentation and whitespace gates apply before publication.
Uninjectable attribute/final-path/volume-root failures retain the stated
inspection-based evidence limit.
