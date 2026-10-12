# Windows Mapping and Enumeration Composition

## Status and bounded scope

Design proposal following the accepted production function composition plan.
The source baseline is 3f43548a03ad75d48b56ded3bbd5ed0ece7ce536, including #214.
This slice covers Windows N07/N08: random-file mapping admission and publication,
and directory enumeration. POSIX #216 has separate production/test/design paths.
Windows N09 (failed positioned-read retirement) remains a separate frontier:
its stack/buffer lifetime and cancellation/wait contract must not be mixed into
this slice.

Only `src/platform/windows_file_system.cc`, its existing unit test file and this
design should change in implementation. Do not change public headers, the private
native fault-seam interface, POSIX code, benchmarks or coverage policy.
Do not certify the whole native file or its deeper helpers as complete.

## Existing contracts and prior art

Reuse OpenRegularFile, ScopedHandle, CloseHandle, PendingWindowsMapping and the
mapped/copied owner classes. Existing native tests exercise hint guards, slot
exhaustion/reuse, concurrent slot reservation, mapping failures, owner lifetimes,
enumeration failure and enumeration-close failure.
They do not yet establish every exact call prefix or primary-error priority.

Mapping is deliberately not the POSIX transaction:

- Open and validate the regular file before mapping policy. No limiter, no hint,
  zero or an unrepresentable hint chooses copied I/O without acquiring a slot.
- Acquire a slot before querying size. An exhausted budget must not call Size.
  Failed Size is an I/O error; negative or unequal size chooses copied I/O.
- The pending slot guard lives in the parent while constructing a mismatched-size
  copied owner. Allocation failure must not prematurely release a slot or leak
  the acquisition handle.
- Create the section, map its view, register the view with the pending guard,
  explicitly close the section, then explicitly close the file. Each failed
  stage suppresses all later stages and preserves its original error.
- Allocate the mapped owner before committing the pending guard. Every earlier
  return or exception releases the slot and unmaps any registered view.
  Destruction preserves section-before-pending-before-file order where relevant.
- Keep original section/file close-error copies at their original logical sites.
  Move errors only through newly introduced propagation envelopes.

Enumeration is likewise not the POSIX close transaction:

- Convert the native path, validate a directory with OpenDirectory and explicitly
  close that acquisition handle before Find. A failed close suppresses Find.
- Find receives the existing native `path / L"*"` spelling. Only
  ERROR_FILE_NOT_FOUND means an empty listing; other Find errors are fatal.
- Process Find's first entry before Next. Exclude exactly `.` and `..`; append
  owned native paths before reusing WIN32_FIND_DATAW.
- Capture GetLastError immediately after the failed Next, before any helper
  cleanup or error construction. Only ERROR_NO_MORE_FILES is successful EOF.
- A Next failure constructs its original list error while the enumeration guard
  is still alive. Guard cleanup does not replace that primary error.
- At successful EOF release the guard and explicitly CloseFind exactly once.
  An explicit close failure is fatal, not successful partial output. Exceptions
  during path/vector allocation close through the guard.

## Recursive definition ledger

Names may adapt to local terminology; responsibilities and ownership may not.

| Definition | Named children or leaf stopping reason |
|---|---|
| WindowsFileSystem::OpenRandomAccess | OpenRegularFile, copied fallback, hint length, slot acquisition, matching-size query and mapped publication; parent owns file and PendingWindowsMapping across fallback construction. |
| OpenCopiedRandomFile | Allocate the existing copied owner and transfer its scoped handle under the original constructor/exception semantics; no new policy. |
| MappingLength | One pure representability decision: present, nonzero, size_t and signed-64-bit bounds. No native query or slot acquisition. |
| MatchesMappingSize | One checked native observation: Size failure preserves mapped-size error; negative or unequal observations return false. |
| CreateMappedRandomFile | Section acquisition, registered view acquisition, ordered handle closing and mapped-owner construction/commit; borrows parent file and pending guard. |
| OpenMappingSection | One scoped native section acquisition and validity/error translation. |
| MapSectionView | One rollback-safe native view acquisition: report failure before registering, otherwise register the returned view immediately. |
| CloseMappingHandles | Compose CloseHandle for section then file, preserving both existing error-copy sites and suppression of later closes. |
| WindowsFileSystem::ListDirectory | Existing native-path/directory acquisition and close; first-find dispatch; guard; child traversal; terminal-error validation; explicit close and owned output publication. |
| AppendDirectoryChild | One owned native-name publication policy, excluding only the two navigation entries. A temporary borrowed name never escapes the call. |
| ReadDirectoryChildren | Append first/current entry then advance with Next; return captured terminal DWORD, not Status, so error construction stays at the original parent boundary. |
| FindCloser::operator() | One best-effort enumeration close on unwinding; move to file scope without changing the borrowed operations lifetime or policy. |

Use concrete helpers, not callbacks, parameter bags or generic resource
frameworks. A native acquisition plus validation/ownership registration is one
domain operation, not a forwarding wrapper. Standard construction, Acquire,
Commit and existing CloseHandle remain children without gratuitous wrappers.
No Status/Result values inside boolean chains or conditional expressions.

## Characterization before production edits

Extend only the test-local existing FaultOperations implementation or add a
test-local recording derivative. Keep the production seam unchanged. Record
handles/call order and explicit Win32 errors without performing allocation in
noexcept native overrides: use fixed-capacity counters/records.

Required cases on the unchanged production implementation:

1. Hint/budget matrix verifies no Size/Mapping/Map for disabled, absent, zero,
   oversized or exhausted choices. Negative/unequal observations perform Size
   but no Mapping; copied owners remain readable and the released slot is
   reusable. Retain existing concurrency and native-owner cases.
2. Mapping success records Open/Inspect/Size/Mapping/Map/section-close/file-close,
   one live view and no acquisition handles. Each injected Size/Mapping/Map and
   first/second close failure asserts exact primary operation/error and call
   prefix, no leaked resource and subsequent slot reuse. Combine primary
   failures with cleanup-close failures to establish error priority.
3. Enumeration verifies first-entry publication, dot filtering, owned Unicode
   names after native-buffer reuse, exact first/next/close calls and no partial
   output on terminal failure. Cover Find-not-found versus other Find errors,
   directory-close suppression, Next failure plus cleanup-close failure, and
   successful EOF followed by explicit close failure.

Prefer deterministic test-local native responses for exact order/name/error
cases, alongside existing real native I/O tests. Synthetic find handles must
never reach the real FindClose implementation. Allocation failures and real
kernel cancellation remain inspection-based, not falsely claimed as injected.

## Delivery and verification

First independently review and accept this design-only PR.
Then add reviewed characterization commits and execute native Windows Debug
cases on unchanged production source. Only afterward implement a mapping topic
and an enumeration topic, each a small commit. Run targeted intermediate native
cases after each topic.

Compare original/final unique native test case sets including WindowsFileSystem,
WindowsFileSystemDeath, Table and Recovery using the repository's actual suite
names; require all new cases, no failures/skips and exact case-set equality.
Use the existing MSVC environment/build/test commands, formatting,
documentation checks and whitespace checks. Do not substitute Linux/POSIX
execution for native Windows coverage.

Independently review the combined source/test/design diff before pushing.
Require all exact reviewed-head CI checks to succeed, including authoritative
GCC 13 coverage for affected shared builds, with no new exclusions.
Normal merge only; verify the reviewed changed paths persist in main.
Record native evidence, review limits and delivery status here without
retrospectively claiming the entire production-wide recursive audit is complete.

## Independent design review

Bounded read-only review found no significant issue in the complete proposal
and verified that its relevant production/test paths remain unchanged against
the specified baseline. It confirmed meaningful child responsibilities and
the transaction, error-copy, native-name and cleanup-priority constraints.
The reviewer did not edit files, execute builds/tests or launch nested agents.
This is design acceptance evidence, not native implementation verification;
allocation failures remain inspection-based.
