# ADR-0064: Windows Filesystem and Sequential Delivery

## Status

Proposed on 2026-10-08. This is a design-only change, not an implemented
Windows backend or an assertion of new durability guarantees.

Independent design reviews precede owner review. Production implementation
must not begin until this design PR has been reviewed and merged, following
[ADR-0006](0006-sequential-pull-request-workflow.md). Each subsequent code
slice receives separate storage-correctness and Windows/integration reviews.
The independent design reviews and focused closure pass below are complete;
owner acceptance and all implementation remain pending.

## Context and current callers

Portable components already build in the Windows Debug/Release CI matrix.
However, `DatabaseEngine::Open` creates only a default `PosixFileSystem`.
Without an injected internal filesystem, Windows returns `NotSupported`;
the public API has no filesystem injection facility. The current Windows
public test verifies that rejection, not a working disk database.

The existing internal interfaces are sufficient:

| Caller | Required Windows behavior |
|---|---|
| WAL and MANIFEST readers | Sequential reads, short reads, EOF, typed errors |
| SSTable reader and table cache | Concurrent positioned reads with owned handles |
| WAL, MANIFEST, and SSTable writers | Buffered append, explicit flush/sync/close |
| Recovery and obsolete-file cleanup | Directory enumeration, size, existence, removal |
| VersionSet CURRENT installation | Same-directory replacement without delete-first |
| Database opening | Directory creation and nonblocking exclusive LOCK |
| Flush, compaction, recovery, VersionSet | Explicit namespace durability boundaries |
| Offline diagnostics | Copied reads and checked output; no database opening |

Clock, serial executor, byte codecs, WAL/SST formats, caches, snapshots,
write grouping, and compaction algorithms remain portable and reusable.
Windows support does not justify another scheduler, a combined Env object,
or changes to persistent formats.

## Prior art and evidence

The locally available original LevelDB matches the pinned reference revision
`7ee830d02b623e8ffe0b95d59a74db1e58da04c5`. Its `util/env_windows.cc` provides
separate file objects, a 64 KiB writer buffer, file flushing, native locks,
same-directory replacement, and a bounded mapping policy. Adopt that
separation and buffering policy, not the whole Env implementation.

Concrete differences that require deliberate treatment:

- Original LevelDB uses ANSI path APIs. Modern accepts native
  `std::filesystem::path`, so the Windows backend uses explicit W APIs.
- Its unbuffered writer narrows the request to DWORD and does not loop over
  successful short writes. Modern's existing Append contract requires all
  bytes or a retained first error.
- Its mapped-read fallback and error classification differ from Modern's
  optional borrowed-view and typed-error contracts.
- Its Windows Sync flushes a file and assumes no separate parent-directory
  sync is required. Modern explicitly checks SyncDirectory before publishing
  newly referenced files and after installing CURRENT.
- Its Windows test demonstrates mapping-budget fallback, but is not a
  complete filesystem, crash-consistency, or Unicode test suite.

Additional mature-library evidence, inspected at immutable revisions:

- SQLite `b0db2db98870acc7917329bc904ee1265e906bf6`, `src/os_win.c`:
  `winSync` checks FlushFileBuffers; `winDelete` does not use its syncDir
  argument. Adopt native file flushing and Unicode conversion practice,
  not SQLite's database-level protocol or its directory policy.
- RocksDB `dec67990f0352352f53cb28b0b53651e73039d95`,
  `port/win/io_win.cc`: Windows file operations use native handles;
  `WinDirectory::Fsync` returns success without a directory flush.
  This is evidence of a platform policy, not proof of Modern's stronger
  directory-entry invariant.

Microsoft's API contracts distinguish synchronous and overlapped reads,
sharing/delete permissions, file flushing, and replacement. In particular,
volume-wide flushing requires administrative privileges; it is not a
reasonable embedded-library substitute for directory fsync.
MOVEFILE_WRITE_THROUGH is not used as proof of a general directory barrier.
REPLACEFILE_WRITE_THROUGH is documented as unsupported.

The C++ standard library supplies native paths and RAII-friendly value types,
but not durable file synchronization or database locks. Win32 supplies those
mechanisms beneath the existing FileSystem interface. No copied upstream
implementation, public VFS registry, or new general-purpose I/O framework is
needed.

## Decision 1: Bound the first supported platform

The first database backend targets Windows 10/11 desktop, x64, local fixed
NTFS volumes, ordinary privileges, and copied SSTable reads. MSVC is the
required initial compiler; clang-cl is an additional gate only after its
actual compiler, C++23 library, and dependency combination is validated.
Select the backend using a compiler/target capability check, not WIN32 or
pointer size alone: x64 and ARM64 both have 64-bit pointers. Other Windows
targets/toolchains retain the portable build and missing-default-backend
NotSupported behavior until separately admitted. They must not acquire
unvalidated production backend sources or native backend tests by accident.

The owned backend checks the database location before recovery mutates it.
Resolve the volume from the existing directory or its existing parent when
creating a database. Reject UNC/network locations, mapped network drives,
non-NTFS volumes, and unsupported device namespaces with NotSupported.
Propagate permission and volume-query errors rather than calling them
unsupported or missing. Creating missing ancestors is not added.
Resolve the existing directory/parent through a native directory handle
before checking its final volume, so a junction cannot make an unsupported
target appear to be the local NTFS volume of its lexical path. Freeze the
resolved absolute database path for the open; do not repeatedly reinterpret
its relative/drive-relative form during recovery.

The standalone filesystem methods implement regular-file operations; this
database support check is not a claim to sandbox arbitrary paths. Junctions
and symbolic links resolve through the OS to the same underlying lock file.
Concurrent external mutation of the database directory remains unsupported.

Windows ARM64, 32-bit Windows, ReFS, FAT/exFAT, SMB, cloud-synced storage,
direct I/O, writable mappings, IOCP, async public APIs, cancellation, repair,
backup, and production readiness are outside this slice.

## Decision 2: Make the namespace durability limit explicit

### Preserve the default instead of silently weakening it

ADR-0011 requires a namespace barrier before a durable MANIFEST first
references a new file. The surveyed ordinary-privilege Win32 mechanisms do
not establish an equivalent general directory-entry barrier for this design.
Successful file flushing, NTFS journaling, passing process-crash tests, and
mature libraries' no-op directory syncs do not independently prove it.

Propose one narrowly named public option:

```cpp
// Proposed addition to Options; not present until the integration PR.
bool allow_weak_namespace_durability = false;
```

Its only purpose is to consent to a backend whose namespace durability is
weaker than ADR-0011. It does not disable file Sync or error propagation.

| Backend/option | Open and namespace behavior |
|---|---|
| Owned POSIX, either value | Existing fsync ordering and guarantees remain unchanged |
| Owned Windows, false | NotSupported before creating, locking, or modifying a database |
| Owned Windows, true | Explicit weak namespace mode; checked native operations and file Sync |
| Explicit internal FileSystem | Retains that filesystem's contract; the option does not weaken or wrap it |

The API maps the boolean to an internal DatabaseEngineOptions field. The
engine passes it only to its owned Windows backend. The platform layer must
not include public Options or engine headers. No persistent marker is added:
the option describes this open's guarantee, not a new database format.
Both creation and reopening on Windows require explicit consent, including
when sync_wal_creation is false. That existing option controls WAL creation,
not all metadata transitions, and is not a substitute for consent.

Strict Windows rejection is an intentional remaining limitation. The first
release enables real disk databases through explicit opt-in, not transparent
default parity. A later strict mode needs separate primary-source evidence
and crash/power-failure validation; it must not be unlocked by a passing
process-exit test or a directory FlushFileBuffers call alone.

### Exact weak-mode contract

- Append and Flush retain their existing buffered/kernel visibility meanings.
- Sync drains the writer buffer and checks FlushFileBuffers on the file.
  File/device errors remain errors; no fallback turns a failed flush into
  success.
- SyncDirectory in a strict Windows filesystem validates its path and
  returns NotSupported. In an explicitly weak Windows filesystem it opens
  and verifies the requested directory, reports open/query/close failures,
  and performs no directory persistence barrier. Success is permitted only
  under this selected, documented policy, not inferred from an OS error.
- Weak mode preserves every higher-layer call and its ordering. It does not
  skip existing injected filesystem/fault tests or globally redefine
  SyncDirectory for POSIX.
- A successful sync write flushes WAL contents before publishing memtable
  visibility. Acknowledged writes are tested against abrupt process exit
  and forced termination while Windows remains running.
- No promise is made that namespace changes survive OS crash, power loss,
  storage-controller failure, or later device failure. Lost CURRENT,
  MANIFEST, WAL, or SST names can prevent recovery or lose acknowledged data
  after such events. File Sync does not remove this namespace limitation.
- Unsynced acknowledged writes may be lost even after process-only failure.
  Failure after partial I/O may leave bytes on disk; Result is not rollback.

The implementation updates the option comments, README example, public
contract, and current ADR-0011 platform summary together. The present
design-only PR merely links the proposal and changes no active guarantee.

## Decision 3: Implement the existing filesystem contracts

Add internal `platform/windows_file_system.{h,cc}` selected only for Windows
targets/toolchains passing Decision 1's compiler/target capability check.
Use Win32 directly, explicit W spellings, and noninheritable handles.
Keep windows.h out of public and platform-neutral headers; isolate
NOMINMAX/WIN32_LEAN_AND_MEAN definitions to the Windows implementation.

### Ownership and concurrency

File objects exclusively own their HANDLE through a move-only internal
RAII helper. Find handles use FindClose, not CloseHandle. Every failure
after an acquisition releases the resource; move assignment releases any
previously owned handle before taking another.

FileSystem calls are concurrent-safe without a global operation mutex.
Sequential and writable objects retain external synchronization.
Random-access Read calls may run concurrently; the caller keeps the object
and destination alive until each call returns. Destruction concurrent with
an operation remains unsupported. No engine mutex is added to the backend
and no callback is invoked from it.

| Operation | Selected mechanism and boundary |
|---|---|
| OpenSequential | CreateFileW, GENERIC_READ, OPEN_EXISTING, synchronous ReadFile |
| OpenRandomAccess | CreateFileW with FILE_FLAG_OVERLAPPED; explicit offset per call |
| OpenWritable | GENERIC_WRITE, CREATE_ALWAYS; exclusive writer |
| OpenAppendable | GENERIC_WRITE, OPEN_ALWAYS; checked seek to EOF before appending |
| FileExists | Checked attributes; only missing-path errors produce false |
| ListDirectory | FindFirstFileW/FindNextFileW; child paths, excluding dot entries |
| FileSize | GetFileSizeEx on a temporary metadata handle |
| CreateDirectory | CreateDirectoryW; existing directory succeeds, existing file does not |
| RemoveFile/RemoveDirectory | DeleteFileW/RemoveDirectoryW, with typed failures |
| RenameFile | MoveFileExW with MOVEFILE_REPLACE_EXISTING, without copy/delete fallback |
| SyncDirectory | The explicit strict/weak policy above |
| LockFile | CreateFileW plus immediate exclusive LockFileEx |

File opens verify regular disk files rather than accepting pipes, devices,
or directories as WAL/SST handles. Read handles share READ and DELETE, not
WRITE; writable handles are exclusive. Temporary size/attribute queries
request FILE_READ_ATTRIBUTES, not GENERIC_READ, and share READ | WRITE |
DELETE so they can inspect an owned writable file without requesting data
access that conflicts with its exclusive sharing.

### Read and write boundaries

Empty reads return zero without issuing I/O. Clamp one read request to
DWORD_MAX; a short read is successful and ERROR_HANDLE_EOF maps to zero.
Validate offset and request arithmetic before narrowing; offsets beyond
the signed 64-bit disk-file range return InvalidArgument.

Each random read owns a distinct OVERLAPPED and manual-reset event, with
Offset/OffsetHigh set from the checked offset. Immediate completion and
ERROR_IO_PENDING both reach a terminal result before returning. Pending
operations are completed with GetOverlappedResult, not treated as failures.
The request, event, and caller buffer remain alive until terminal completion;
no error path returns with I/O still referencing them. There is no shared
file-position seek and no event pool or cancellation API in the first slice.

TryReadView initially returns nullopt. allow_mmap_reads remains a permission,
not a promise that every backend maps. Windows uses copied reads even when
the existing option is true; false continues to force copied reads.

Use the existing 64 KiB writer policy. Append loops over bounded WriteFile
requests, advances by the actual byte count, and treats zero progress as Io.
Retain the first write/flush/sync failure. Close flushes unless already failed,
always attempts to release the handle, and returns the first error. A
failure to close without an earlier error is reported. Close is terminal,
does not imply Sync, and is not retried; the destructor only performs
best-effort cleanup when explicit Close was not attempted.

### Paths, enumeration, and errors

Reject empty paths and embedded native null code units. Accept valid native
UTF-16 without routing filesystem access through path.string() or an ANSI
code page. Ordinary relative/absolute DOS paths are resolved with dynamically
sized GetFullPathNameW buffers and represented as absolute extended-length
paths for W calls. Already extended absolute drive paths retain their
meaning; do not blindly prepend another prefix. This is lexical resolution,
not file-identity canonicalization. Do not normalize Unicode or case.
Extended UNC and device paths are not part of first-release database support.
Before resolving an ordinary DOS path, reject non-navigation components
ending in a dot or space with InvalidArgument; do not turn a normally trimmed
name into a different literal name by adding an extended prefix. Components
that are exactly dot or dot-dot retain ordinary navigation semantics.
Already-extended absolute paths retain literal component semantics and are
not subjected to this ordinary-DOS trimming policy. Include both policies
in path tests.

Keep enumeration names native. Generated metadata names are ASCII; recovery
and cleanup must inspect only an ASCII basename before ParseFileName and
ignore unrelated non-ASCII entries without lossy conversion or exceptions.
Use a low-layer UTF-8 path-rendering helper for error messages, sharing or
extracting the existing diagnostic conversion where appropriate. Do not
make engine or platform depend on diagnostics. Valid Unicode directories
must also work on error paths, not just successful opens.

Capture GetLastError immediately, before RAII cleanup or formatting.
Messages contain operation, UTF-8 path, numeric native error, and available
system text; tests assert codes and numbers rather than localized prose.
If a native path cannot be rendered, use an explicit unrepresentable-path
marker without replacing the original OS error. Allocation failures follow
ADR-0004 rather than broad catch-all success fallbacks.

| Condition | Result |
|---|---|
| Missing file/path | NotFound; FileExists returns false |
| Lock/share contention during LockFile | Busy |
| Empty/null path, invalid offset, existing file passed to CreateDirectory | InvalidArgument |
| Unsupported filesystem, device, or strict Windows durability | NotSupported |
| Access denial, media error, disk full, other sharing errors | Io |

Do not map every ERROR_ACCESS_DENIED to Busy or every attributes failure to
NotFound. Enumeration of an empty directory must distinguish it from a
missing/inaccessible directory, and FindNextFile errors other than
ERROR_NO_MORE_FILES must propagate.

## Decision 4: Preserve replacement, locks, and file lifecycle

CURRENT's temporary file is appended, synced, and closed before the native
same-directory replacement. Do not delete CURRENT first, use
MOVEFILE_COPY_ALLOWED, or schedule operations for reboot. Cross-volume
replacement fails rather than becoming a non-atomic copy.
Successful replacement is the installation boundary while the OS remains
running; this design claims no power-failure durability from its flags.
On replacement failure, return the captured error, install no in-memory
version, and follow existing recovery behavior. Do not claim rollback for
arbitrary OS or device failure. Tests cover first installation, repeated
replacement, denied replacement, and reopening.

Lock the existing LOCK file with GENERIC_READ | GENERIC_WRITE and
FILE_SHARE_READ | FILE_SHARE_WRITE, without DELETE sharing. Acquire an
exclusive range from offset zero using LOCKFILE_EXCLUSIVE_LOCK |
LOCKFILE_FAIL_IMMEDIATELY. A zero-initialized OVERLAPPED is local to the
synchronous lock call; the RAII lock owns the successful handle independently
of the FileSystem lifetime. Release with UnlockFileEx and close on destruction.
Native lock acquisition is the exclusion linearization point.

Windows handle-associated locks, unlike POSIX process-associated fcntl
locks, do not require copying the POSIX process-global path reservation
table. Same-path, case aliases, directory junctions, and distinct
FileSystem instances must contend through the underlying file identity.
Closing a failed duplicate attempt must not release the existing lock.
Process death releases the lock through OS handle cleanup; tests wait for
child termination and use a bounded observation deadline.

Recovery and obsolete-file cleanup preserve current live-Version,
pending-output, and table-cache eviction rules. Read handles allow DELETE
sharing so an obsolete copied-read file may be marked for deletion while
an already acquired handle continues reading. Live filenames are not reused
during a successful open; delete-pending state is not treated as permission
to recreate a live table. This does not promise non-reuse across failed opens:
persisted allocation state can select the number of an unpublished recovery
table on the next open.
No new background retry service or broad catch is added.

External readers that deny deletion can cause cleanup to fail. RemoveFile
returns that error; the engine retains its existing best-effort cleanup
policy, so an orphan may remain until a later cleanup/open. Recovery ignores
an unreferenced orphan for live-file selection, but attempting to overwrite a
reused orphan filename can fail with checked sharing or delete-pending errors
while an external reader survives. This is an availability limitation, not
permission to overwrite a live table or a guarantee of reopening success.
Add regressions for a failed recovery's unpublished table, reopening after
its handles are released, and a surviving reader that blocks filename reuse.
The blocked open must report an error and preserve previously live data.
Do not label every leftover file as corruption or silently change cleanup
into a new sticky-error policy.

Mapped-file removal is deliberately deferred: DELETE sharing alone does
not establish POSIX unlink semantics for Windows file mappings. A later
mapping ADR must prove unmap-before-removal and pinned-version lifetime
invariants, or define a bounded cleanup policy before enabling mappings.

## Decision 5: Deliver sequential, testable slices

Only the first two implementation slices are specified in detail here.
Later high-risk work gets its own ADR and design gate; these are dependencies
and acceptance criteria, not approval to implement everything in one PR.

| Slice | Deliverable and exit criteria |
|---|---|
| 1. Native filesystem | Windows backend plus real-file contract/failure tests, strict rejection and explicit weak-mode directory tests; no public database enablement |
| 2. Database integration | Public opt-in, owned backend/preflight, Unicode-safe recovery/cleanup, disk WAL/table/API/engine tests, Windows CI; all existing POSIX behavior retained |
| 3. Compatibility and crash harness | Enable portable extended tiers separately; Windows child-process recovery and lock tests, seeded models, golden and bidirectional original-LevelDB cross-open |
| 4. Diagnostic command | Borrowed Windows stdout/stderr adapter, Unicode CLI input, exact ASCII output, redirected file/pipe failures, existing read-only dump contract |
| 5. Benchmark baseline | Windows copied-read workloads and original-LevelDB comparison with explicit platform/namespace policies; no unsupported CPU capture claims |
| 6. Optional mapped reads | Separate design, baseline comparison, view lifetime/resource/fallback/removal tests; keep copied-read opt-out |
| 7. Optional profiling | Separate design for Windows collection and owned-process lifecycle, symbol provenance and timeout cleanup |

Core engine opening in strict Windows mode fails before any disk mutation;
slice 2 must prove this for both an absent and existing database. No public
filesystem injection is added to bypass that rejection.

CMake source selection gains a WIN32 branch for the backend. Only platforms
that pass the initial compiler/target capability check select its production
sources and native tests. All other Windows targets keep the portable
missing-default-filesystem tests. Only platforms with neither admitted
backend keep missing-default-filesystem tests. Real-disk tests
that do not depend on POSIX syscalls become native-platform tests instead
of being disabled on Windows. fork, mkdtemp, ftruncate, and SIGBUS-specific
tests remain separately selected. Reuse TemporaryDirectory for portable
fixtures rather than copying POSIX-only setup.

Split the root POSIX-only harness guard by actual capability. Model/golden/
differential tests do not need fork; deterministic CrashFileSystem tests
remain portable protocol tests. The current process test does require
fork/_exit/waitpid and needs a Windows helper before its gate is lifted.
Do not unconditionally enable tools, benchmarks, or profiling by deleting
the existing guard.

Keep original LevelDB as a separate private reference target. Local source
may be supplied through the existing FetchContent source override; no
developer machine path is committed. Check the actual MSVC RTTI, exception,
CRT, and codec target combination: the current reference helper repairs
RTTI only for GNU/Clang/AppleClang, not MSVC. No reference compiler policy
may disable exceptions or RTTI in Modern or consumer targets.

Windows unit/integration CI explicitly establishes the MSVC developer
environment for Ninja. Add supported Windows presets rather than using
GCC coverage, sanitizer, or profiling flags on MSVC. Preserve strict warnings,
nonempty CTest selections, consumer tests, documentation-only routing, and
existing Linux/macOS coverage/sanitizer gates. Windows line coverage needs a
separate supported measurement; GCC's Linux report cannot certify the new
Windows backend's branches.

The dump command needs a wide entry point and a defined UTF-8-to-native path
boundary; converting wmain arguments to UTF-8 and then feeding the current
locale-dependent narrow path constructor is not sufficient. Its internal
argument seam must retain native paths or explicitly decode UTF-8 without
changing version-1 output or POSIX byte-path semantics.

The Python profiling runner currently relies on ps, pgrep, POSIX process
groups/signals, and macOS capture. A Windows workload baseline must separate
portable subprocess execution from collection before enabling those tests.
Do not report unsupported capture as an empty successful profile. Native
benchmark binaries and scripts must resolve .exe paths through CMake target
paths or an explicit build manifest, not a guessed extension.

## Verification matrix and failure model

| Boundary | Required evidence before its implementation PR is ready |
|---|---|
| Sequential/random reads | Empty/short/EOF, unaligned offsets, concurrent disjoint reads, immediate/pending completion, checked >32-bit offset |
| Buffered writes | Boundary/bypass sizes, reopen/append, short/zero-progress writes, Flush vs Sync, first-error retention, close after failure |
| Native errors/resources | Missing vs denied, enumeration failure, sync/close failure, resource release after every partial acquisition |
| Paths | ASCII, spaces, Chinese and non-BMP characters, relative/absolute/extended paths, >MAX_PATH, embedded null, ordinary vs extended trailing-dot/space policy, Unicode error rendering, unrelated Unicode children |
| Namespace | Empty listing, existing-directory creation, overwrite replacement, failed replacement, unsupported volume, strict preflight without mutation |
| Locks | Same/cross-instance contention, case/junction aliases, cross-process contention, failed duplicate preserving original, release after exit |
| Database lifecycle | WAL replay, flush/compaction, snapshots and iterators across maintenance, reopen, obsolete-file cleanup, orphan reuse with and without a surviving reader |
| Public compatibility | Default strict rejection, explicit weak-mode open, moved-from errors, retained child-handle lifetimes, unchanged POSIX behavior |
| Formats | Existing golden vectors and original-LevelDB cross-open with None/Snappy/Zstd using ASCII fixture paths |
| Process crash | Parent-observed acknowledged sync writes survive destructor-free exit and forced child termination; interrupted unsynced batch is not falsely required to survive |
| Power-loss simulation | Existing portable strong-protocol model stays unchanged; results are not advertised as real Windows power-loss evidence |
| Build/consumer | MSVC Debug/Release, explicit Ninja environment, private dependency isolation, unchanged disabled-feature consumers, nonempty CTest selection |

Real Win32 calls demonstrate successful native behavior. Deterministic
short-write, pending-read, resource-acquisition, sync, and close errors need
a narrow internal test seam around the backend's OS calls, not only an
in-memory FileSystem substitute. Keep that seam private and limited to
operations actually needed by these failure tests; do not publish a pluggable
Win32 API table or add runtime policy callbacks to the engine.

Process tests use a dedicated executable or explicit child mode, not fork.
The parent receives a bounded ready/acknowledgement message after each
successful sync write and then terminates only that owned child. Wait for
process completion before reopening, close every pipe/process/thread handle,
and fail on timeout rather than accepting missing observations. Lock-test
children inherit no database handles. An in-flight batch may be fully present
or absent; previously acknowledged batches must remain intact.

For every behavioral change, observe the targeted failing test before the
fix and run the smallest affected suite plus applicable consumers. Before
owner review, two independent personas review the exact code diff: storage
correctness/durability/lifetimes and Win32/build/Unicode/test integration.
Assess findings against concrete contracts, add regressions for accepted
bugs, reuse those reviewers for focused follow-up, and record declined
findings with reasons. Clean review means no identified actionable findings
remain, not a proof of correctness.

## Consequences and owner-review boundary

The first release gains explicitly opted-in Windows databases without
altering formats or pretending to provide POSIX namespace durability.
The public boolean is the only proposed API addition; it is justified by
an existing stronger default contract and applies only where needed.
Strict default Windows opening, real power-loss parity, mappings, tools,
and profiling are not all solved by the first backend PR.

Owner review must approve the weak-mode boundary and sequential slices before
production work begins. If that boundary is unacceptable, retain Windows
NotSupported and investigate a separately justified strict namespace
mechanism; do not implement a silent no-op or use elevated volume flushing.

## Design review record

Two independent personas performed bounded static reviews at the owner's
request: storage correctness/durability/lifetimes and Win32/Unicode/build/test
integration. A focused architecture/contract closure pass checked the accepted
corrections, not an implementation that does not yet exist.

| Finding | Disposition |
|---|---|
| Filename non-reuse is false across failed recovery | Accepted: qualify the invariant, document sharing-related availability failure, and require orphan-reuse regressions |
| WIN32/pointer size does not enforce x64/MSVC scope | Accepted: require explicit target/compiler admission and retain missing-backend behavior elsewhere |
| Extended prefixes can change ordinary dot/space suffix semantics | Accepted clarification: reject ambiguous ordinary components and preserve explicit extended literal paths |
| A stale WIN32-only directive contradicted the revised target gate | Accepted in closure: make Decision 3 use Decision 1's capability check |

The closure reviewer confirmed those corrections and the coupled
handle-resolved volume, frozen absolute path, and metadata-access amendments.
No actionable finding remains from these bounded passes. Strict default
rejection, per-open weak-mode consent, and deferred power-loss/mapping parity
remain deliberate owner-review tradeoffs, not rejected bug reports.

Documentation checks cover ASCII English for this new ADR, balanced fences,
local links, whitespace, and the exact documentation-only changed-path set.
The seven DocumentationPathsTest/RoutingOutputTest cases pass on Windows.
The existing complete CI-routing suite has a Windows-incompatible newline
filename in its GitChangeScopeTest fixture; the full Linux CI gate remains
required. No tests were changed to hide that limitation, and no production
build, native backend execution, or power-loss evidence is claimed here.

## References

- [ADR-0004 errors and ownership](0004-errors-ownership-and-runtime.md)
- [ADR-0006 sequential review gates](0006-sequential-pull-request-workflow.md)
- [ADR-0011 filesystem and durability](0011-filesystem-contracts.md)
- [ADR-0040 compatibility and crash harness](0040-compatibility-and-crash-harness.md)
- [ADR-0056 mapped-table lifetime policy](0056-leveldb-table-mmap-parity.md)
- [ADR-0062 diagnostics and output](0062-read-only-storage-file-diagnostics.md)
- [Implementation dependency DAG](../dependency-dag.md)
- [Pinned LevelDB Windows backend](https://github.com/google/leveldb/blob/7ee830d02b623e8ffe0b95d59a74db1e58da04c5/util/env_windows.cc)
- [Pinned LevelDB Windows tests](https://github.com/google/leveldb/blob/7ee830d02b623e8ffe0b95d59a74db1e58da04c5/util/env_windows_test.cc)
- [SQLite Windows VFS](https://github.com/sqlite/sqlite/blob/b0db2db98870acc7917329bc904ee1265e906bf6/src/os_win.c)
- [RocksDB Windows I/O](https://github.com/facebook/rocksdb/blob/dec67990f0352352f53cb28b0b53651e73039d95/port/win/io_win.cc)
- [CreateFileW](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-createfilew)
- [ReadFile](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-readfile)
- [FlushFileBuffers](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-flushfilebuffers)
- [MoveFileExW](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-movefileexw)
- [ReplaceFileW](https://learn.microsoft.com/en-us/windows/win32/api/winbase/nf-winbase-replacefilew)
- [DeleteFileW](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-deletefilew)
- [LockFileEx](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-lockfileex)
- [GetFileSizeEx](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-getfilesizeex)
- [Windows path namespaces](https://learn.microsoft.com/en-us/windows/win32/fileio/naming-a-file)
