# Platform Support and Durability

[User reference](README.md)

Modern LevelDB preserves one logical database format while platform backends
provide different native I/O and durability mechanisms.

## Admitted platforms

| Platform | Admitted boundary | Table access |
|---|---|---|
| Linux/macOS | Supported POSIX toolchains used by repository CI | Read-only mmap by default; copied `pread` control/fallback |
| Windows | Windows 10/11 desktop, native x64 MSVC, local fixed NTFS, ordinary privileges | Read-only file mapping by default; copied `ReadFile` control/fallback |

Windows ARM64/ARM64EC, 32-bit Windows, clang-cl/MinGW, ReFS, FAT/exFAT,
SMB/network paths, mapped network drives, and cloud-synced storage are not
admitted by the owned native backend.

## Namespace durability

POSIX uses file sync plus an actual directory namespace barrier where the
engine contract requires one.

Ordinary-privilege Windows APIs in the admitted design do not establish the
same general directory-entry persistence barrier. Therefore every owned native
Windows open requires:

```cpp
options.allow_weak_namespace_durability = true;
```

Without consent, opening fails with `NotSupported` before creating, locking,
or modifying the database.

With consent:

- successful file `Sync` still checks `FlushFileBuffers`;
- a sync write flushes WAL bytes before acknowledgement;
- process exit/forced-termination recovery and lock release are tested;
- namespace changes do not gain a POSIX-equivalent power-loss guarantee.

An OS crash, power loss, controller failure, or later device failure can lose
file names, acknowledged data, or recoverability. `sync_wal_creation=false`
does not waive weak-namespace consent.

## File sync versus namespace sync

| Operation | What it establishes |
|---|---|
| `WritableFile::Flush` | Drains the library's write buffer to the OS |
| `WritableFile::Sync` | Requests durable file contents through the backend |
| `SyncDirectory` on POSIX | Requests persistence of directory namespace changes |
| `SyncDirectory` in Windows weak mode | Validates the directory/handle and ordering, but performs no persistence barrier |

Never describe file-content sync alone as durable installation of a new WAL,
MANIFEST, `CURRENT`, or SSTable name.

## Mapped and copied reads

`allow_mmap_reads=true` permits the backend to map exact-size immutable table
files under a shared 1,000-mapping budget. Missing/mismatched size hints,
empty files, exhausted slots, or explicit opt-out use copied reads.

Mapping avoids a copy; it does not eliminate page faults or storage errors.
A later mapped-page fault can terminate the process:

- POSIX: typically `SIGBUS`;
- Windows: an in-page exception.

Set `allow_mmap_reads=false` when typed `Io` read errors are required.
External modification or truncation of live mapped tables is unsupported.

## Paths and locks

The POSIX backend uses native path bytes. The Windows backend uses wide Win32
APIs and supports Unicode, non-BMP, extended, and long paths within the
admitted storage boundary. Ordinary DOS components ending in a dot or space
are rejected; explicit extended paths retain literal semantics.

The database `LOCK` excludes aliases that resolve to the same underlying file.
Process death releases native handles/locks, but one process must still own an
open database directory exclusively.

## Compatibility evidence

The repository retains:

- LevelDB golden vectors and bidirectional cross-open tests;
- deterministic power-loss protocol tests;
- native Windows process-crash and lock tests;
- mapped/copied comparative benchmarks;
- selected workload and CPU-attribution evidence.

Those checks are evidence within their stated models. They are not real
power-loss certification or production-readiness certification.
