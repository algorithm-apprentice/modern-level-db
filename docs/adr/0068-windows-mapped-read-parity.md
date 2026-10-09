# ADR-0068: Windows Mapped-Read Reference Parity

- Status: Implemented

## Status and scope

Design accepted and merged by PR #106 on 2026-10-08 after the copied-read
Windows baseline in PR #105. The native mapping slice is implemented.
The owner requires alignment with the pinned original Windows implementation
before adding independent resource policies or claiming performance parity.
Independent storage/lifetime and Win32/build/policy design reviews precede
production implementation and sequential merge.

Implement `implement-windows-mapped-reads` from ADR-0064. Reuse the existing
optional exact `RandomAccessFile::TryReadView` and table/block ownership
contracts, not a new public mapping interface. Scope remains desktop Windows
10/11, native MSVC x64, local fixed NTFS and explicit weak namespace consent.
No stronger durability, writable mappings, partial-file windows, async I/O,
external mutation tolerance or broader compiler/filesystem admission follows.

## Existing contracts and evidence

The native backend currently retains a noninheritable read handle with
READ and DELETE sharing, but not WRITE sharing. Concurrent copied reads own
their OVERLAPPED/event until terminal completion. `OpenRandomAccess` accepts
the MANIFEST size hint but does not currently use it for mapping.

ADR-0056 already supplies borrowed uncompressed blocks, owning decompressed
blocks, table-cache pins, and file destruction after dependent blocks.
Uncompressed borrowed blocks bypass the block cache. Do not duplicate that
engine logic or substitute a scratch buffer whose next read invalidates a view.
The present POSIX limiter is count-only. The Windows implementation must match
the pinned native reference rather than introduce another independent policy.

The supplied original LevelDB checkout is clean at revision
`7ee830d02b623e8ffe0b95d59a74db1e58da04c5`. Its `util/env_windows.cc` supplies
the following field/function correspondence:

| Reference mechanism | Modern Windows decision |
|---|---|
| `kDefaultMmapLimit` and process Env limiter | Default 1,000 mapped files on admitted x64; no new byte cap |
| `Limiter::Acquire/Release` | Relaxed atomic count reservation, restore unsuccessful acquisition, release after unmap |
| `WindowsEnv::NewRandomAccessFile` | Default read-only mapping for exact-size SSTs; copied control and exhausted-limiter fallback |
| `CreateFileMappingA` / `MapViewOfFile` | Unnamed PAGE_READONLY / FILE_MAP_READ whole-file mapping through W APIs |
| temporary file and section handles | Close acquisition handles after successful view creation; view owns the mapping reference |
| `WindowsMmapReadableFile::Read` | Exact borrowed `TryReadView`; existing table/block ownership avoids hot-path copies |
| `WindowsMmapReadableFile` destructor | Unmap before returning the count slot |
| size/section/view acquisition failure | Checked native error after resource rollback, not an unreported copied fallback |
| `kWritableFileBufferSize` | Existing 64 KiB buffered writer, unchanged |

No implementation is copied wholesale. Retain required Modern contracts:
native Unicode paths, checked short writes and sticky errors, safe range
arithmetic, typed copied reads, namespace consent and file-format validation.
Document each necessary difference instead of quietly altering the hot path.

A native read-only NTFS probe retained the file handle, created an unnamed
PAGE_READONLY section and FILE_MAP_READ view, and observed:

- a competing write open failed with ERROR_SHARING_VIOLATION;
- same-directory rename and DeleteFile succeeded with DELETE sharing;
- the original bytes remained readable until explicit unmapping.

Repeating the probe after closing both acquisition handles, as the reference
does, observed a successful write-handle open without modifying the file,
successful rename/removal, and the original view bytes surviving until unmap.
That confirms why external mutation must remain outside the mapped contract.

This is process-running evidence, not a promise that filename reuse always
succeeds while a mapped section survives. Microsoft documents that mapping
references can hold a file open without retaining the original handle's
sharing restrictions. The reference closes its acquisition handles after
success; Modern does the same. Published SST immutability is enforced by
engine ownership, not additional per-mapping handles. External modification
or truncation of a live table remains unsupported, as in ADR-0056.

## Decision 1: Match the native mapped-read default

The existing `allow_mmap_reads` option selects mapping eligibility; do not
add another overlapping public boolean. Its existing true default remains
true on Windows and POSIX. Native Windows now honors that option, rather than
leaving it ineffective. False is the explicit copied-read control for typed
I/O errors. Update documentation, provenance and exact default tests together.
This intentional transition from the first-release copied implementation
requires native lifetime/correctness and comparative measurements before merge.

The owned filesystem receives both independent policies: weak namespace
consent and mapping eligibility. Mapping consent never waives namespace
consent. Direct internal filesystem construction uses the same mapped default,
with an explicit copied control and isolated test limiter injection.
No database format marker or persisted mapping policy is added.

Both copied and mapped Windows opens still require the same resolved-volume
preflight. Existing platforms and injected filesystems retain their contracts.

## Decision 2: Preserve the count-only native acquisition path

Use CreateFileMappingW with PAGE_READONLY, zero maximum-size fields and no
name, then MapViewOfFile with FILE_MAP_READ, zero offset and the full size.
Do not extend, modify, execute, pre-touch or flush an immutable table.

Map only when the option is enabled, an expected size is present, the actual
handle size equals that expectation, and the size is nonzero and fits size_t
and the accepted signed file-offset range. Missing hints, empty or mismatched
files and unavailable count slots select the existing copied reader on the
same handle. The expected-size guard is a required Modern format contract,
not an additional limit on valid SSTs. Base open, path and kind-query errors
remain typed errors. After acquiring a slot, native size/section/view failures
return their captured error after rollback, matching the reference failure
policy. Never misreport failed mapping as successful mapped capability.

All production instances share a process-wide limit of 1,000 mapped files,
matching the admitted 64-bit reference. Do not add a 4 GiB byte limit, a
reservation mutex or per-read accounting absent from the reference.
Each unsuccessful acquisition rolls back exactly once. Tests can inject an
isolated count limit without mutating the process default. The mapped object
keeps its limiter alive after its filesystem is destroyed.
Reuse the existing count-limiter mechanism or extract it unchanged for both
platforms; do not duplicate a new generic I/O framework.

Add only necessary section/map/unmap/size operations to the existing private
fault seam. No public Win32 API registry or new portable VFS abstraction is
introduced. Retain normal copied-read OVERLAPPED ownership on every fallback.

## Decision 3: Preserve reference view ownership and existing table pins

The mapped file object owns the view and its limiter reservation. It exposes
immutable exact views and implements copied Read using the same range/EOF/invalid-offset
semantics as the existing reader. Check ranges by subtraction after checking
the offset; do not permit integer overflow. Concurrent views/reads do not
invalidate each other, and destruction concurrent with a read is unsupported.

Close the temporary section and original file handles after view acquisition
and before returning the mapped object, as the reference does. Explicit close
failures are checked and abort the acquisition after unmapping/rollback;
they must not leak handles or pretend successful open.
On destruction, unmap first and release the reservation only after its view
is gone. Allocation
failure follows ADR-0004; acquisition guards own rollback until a fully
constructed object accepts ownership. Cleanup must neither double-unmap nor
recycle a reservation while a view remains. Failed unmapping of an internally
owned valid base is an invariant failure and terminates explicitly, rather
than silently exceeding the mapping budget. A bounded death regression tests
that failure path; normal lifecycle tests verify actual resource release.

Recovery, table eviction and obsolete cleanup keep their existing
live-Version/pending-output/pin rules. DELETE sharing is not a POSIX unlink
promise: preserve checked native removal/replacement errors, and do not
overwrite a still-live table or start a background retry service.
Failed-open orphan reuse can remain blocked until external mapped readers
release their resources, just as documented for copied readers in ADR-0064.

## Decision 4: State mapped-page failures honestly

Mapping creation success does not guarantee later page access succeeds.
Microsoft documents EXCEPTION_IN_PAGE_ERROR from accessing file-backed views.
Stable borrowed spans are accessed throughout existing decoders/iterators;
wrapping only Read in SEH would not make those accesses return typed Io.

This mapped default consequently permits process termination on a mapped-page storage
fault, analogously to POSIX SIGBUS. Do not install a process-wide exception
handler, hide a page fault as EOF or promise a fallback after a borrowed span
has already escaped. Document that false is the copied-read control for typed
I/O errors. The public default transition is explicit; it does not establish
production readiness or stronger durability. Existing copied-control
measurements remain available and labeled rather than being overwritten.

## Verification and comparison gate

Observe a requested native mapped view unavailable before implementation.
Required native tests cover:

- exact/empty/out-of-range/overflow views, copied Read EOF and invalid offsets;
- concurrent immutable views and reads, no retained acquisition handles;
- count exhaustion, shared limiter, release/reacquisition, and a mapped object
  surviving its filesystem;
- wrong/missing hints, empty files, failed size/section/view acquisition,
  explicit acquisition errors and no leaked handles/views/reservations;
- actual cleanup ordering and explicit unmap-invariant failure;
- Unicode and extended paths, rename/removal while pinned, checked filename
  reuse and successful reuse after reader release;
- a real uncompressed SST with borrowed cacheability, existing compressed
  paths, table-cache eviction pins, snapshots/iterators through maintenance,
  and reopen through both default-mapped and copied controls.

Preserve the ordinary copied benchmark and its policy/artifact names. Add a
separate native default-mapped benchmark binary from the identical source and
workload, with only its Modern option selection changed. Its policy says
`mapped_default`, not that every file necessarily maps. Publish separately
named samples, final policy and manifest; bind each binary to its own run.
Both native modes remain diagnostic-only. Legacy sample schema, POSIX default
20x gate, records, seeds, alternating order and timing boundaries stay intact.
Compare copied, mapped and the pinned reference on the same machine and
workload, with compression, cache/work-buffer settings, warm-up and durability
choices recorded. Native selected-workload profiling must additionally use
the existing explicit LevelDB-compatible WAL-creation control when measuring
write parity. Identify any unexplained regression before accepting the
candidate; correctness-only success is not evidence of performance parity.
Do not invent a threshold, report a cold-cache/sync-write SLA, or assume API
similarity itself proves equal speed.

Extend native CI to run both modes and publish only the final refreshed
policies. Require full Debug/Release and extended native regressions, existing
POSIX benchmark/profiling/coverage/sanitizer/fuzz/consumer checks, separate code
personas and focused closure before merging the implementation.

## Design review record

Independent storage/lifetime and Win32/build/performance-policy personas
reviewed the revised reference-parity contract. Both reported no actionable
high-confidence blockers after checking the pinned native field/function
correspondence, view ownership, acquisition rollback, default policy,
format/pin safeguards and comparison requirements.

The earlier unpublished byte-cap/copied-default/retained-handle proposal was
superseded by owner direction, not accepted as native parity. Its rounded-byte
closure is not an approval of this different contract. Native probes and the
before-implementation Modern capability observation were collected separately
from the read-only design reviews.

## Implementation evidence and code review

The implementation extracts the existing relaxed-atomic count limiter for
both native platforms without changing its policy. Windows honors the public
mapped default, returns exact stable borrowed views, closes acquisition
handles, and reports native acquisition errors after rollback. No byte cap,
reservation mutex or extra per-view file/section handle was added.

Native validation passed 41 filesystem/lifecycle cases, full Debug 718 and
Release 714 cases, 18 extended model/cross-open/process cases, 14 checker
contracts and all four copied/mapped/provenance benchmark CTests.
Full-suite validation exposed the copied-fixture helper's old ignored-option
assumption; the helper now explicitly disables mapping instead of weakening
its copied-block assertions. Separate real native SST cases prove borrowed
uncompressed cache bypass and owning Snappy/Zstd decompression.

Idle-machine measurements used the same source/workload with distinct binaries
differing in Modern file-access selection, with independently checked binary/sample/policy
bindings. At 65,536 entries and five alternating trials, copied read/scan
medians were approximately 3.23x/3.09x the native reference; default-mapped
medians were approximately 1.14x/1.16x. The smaller 4,096-entry baseline showed
mapped read/scan at approximately 1.72x/0.93x. These are local diagnostic
observations, not universal admission or equal-speed claims. Mapping removes
the dominant large-workload file-access gap; residual workload/API differences
remain for the separate native profiling and attribution slice.

Independent storage/lifetime and Win32/build/mode-provenance code personas
reviewed the exact implementation, shared limiter, fixture correction and
fault/concurrency coverage. Both reported no actionable high-confidence bug.
Runtime execution and performance observations were collected separately
from those read-only reviews. POSIX gates and persistent formats are unchanged.

## References

- [Sequential review](0006-sequential-pull-request-workflow.md)
- [Allocation contract](0004-errors-ownership-and-runtime.md)
- [Mapped table/block ownership](0056-leveldb-table-mmap-parity.md)
- [Windows lifecycle and consent](0064-windows-filesystem-and-delivery.md)
- [Copied native benchmark](0067-windows-comparative-benchmark-baseline.md)
- [Native backend](../../src/platform/windows_file_system.cc)
- [Table-cache pins](../../src/engine/table_cache.cc)
- [CreateFileMappingW](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-createfilemappingw)
- [MapViewOfFile](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-mapviewoffile)
- [UnmapViewOfFile](https://learn.microsoft.com/en-us/windows/win32/api/memoryapi/nf-memoryapi-unmapviewoffile)
- [DeleteFileW](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-deletefilew)
