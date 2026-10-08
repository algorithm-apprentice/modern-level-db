# ADR-0066: Native Windows Diagnostic Command

## Status and scope

Design accepted by PR #102 on 2026-10-08 after Windows verification PR #101.
The native diagnostic slice is implemented. This records the design gate
for `implement-windows-diagnostics` in ADR-0064.
Independent diagnostic-contract and Win32/Unicode/output reviews precede
implementation and sequential merge.

The command remains:

```text
modern_leveldb_tool dump FILE...
```

Extend the existing offline diagnostic command to the admitted x64/MSVC
Windows backend. Do not add a database opener, online dump, backup/export,
repair, new persistent format, new diagnostic schema, or mapping policy.
All of [ADR-0062](0062-read-only-storage-file-diagnostics.md)'s corruption,
read-only, escaping, traversal, sink-aliasing, and return-code contracts remain.

## Current callers and prior art

`RunDiagnosticTool` currently receives narrow string views, parses fixed ASCII
flags, constructs native paths, and feeds the production `DumpFile` decoders.
The standalone main selects a copied POSIX filesystem and borrowed descriptor
sinks; its Append uses complete checked unbuffered writes. DumpFile never
flushes, syncs, or closes those borrowed sinks.

Adopt the same checked, borrowed-output design on Windows. Native std handles
and WriteFile provide file/pipe/console output without a C-runtime text-mode
translation. Original LevelDB's utility remains the command-shape prior art,
not the Unicode conversion boundary: the pinned Windows Env uses ANSI calls.
The already implemented Windows filesystem supplies W APIs and copied reads.

## Decision 1: Preserve native paths at the command seam

Keep the existing narrow-argument function and its POSIX byte-path behavior.
Add an internal native-path entry point, with no public library header:

```cpp
int RunNativeDiagnosticTool(std::span<const std::filesystem::path> arguments,
                            FileSystem& file_system,
                            WritableFile& output, WritableFile& errors);
```

Both entry points use one shared execution loop for DumpFile, escaped errors,
continue-on-input-error behavior, and stopping after a failed output stream.
Share parsing helpers where doing so is simpler than duplication; do not add
a general parser framework, public locale switch, or polymorphic argv API.

The Windows executable uses wmain and native wide argv. It constructs
filesystem::path directly from those arguments. Do not convert them to UTF-8
and feed the locale-dependent narrow constructor, and do not use
generic_u8string as a round-trip codec: it is the existing diagnostic display
projection, not an identity-preserving native argument representation.

Fixed commands and flags remain ASCII: --help, dump, and leading '-' checks.
Validate all arguments before reading the first input, retaining existing
unknown-command/missing-file/option return behavior.
The native entry point retains each final path including explicit extended
prefixes; paths reach the native filesystem without ANSI conversion.
Diagnostic paths still use the version-1 generic UTF-8 escaped display.

All existing narrow tests remain valid. Native Unicode tests use real files
or native-safe fixtures rather than teaching MemoryFileSystem to hide
locale conversion failures.

## Decision 2: Borrow native output handles, never own them

Add an internal WindowsOutputFile adapter using borrowed GetStdHandle values.
Keep Windows headers out of public/neutral headers; a Windows-only internal
header may reuse the existing private OS wrapper/fault seam.
The caller keeps each borrowed handle valid for the adapter's lifetime.
Only synchronous console/file/pipe handles are in contract. Arbitrary
FILE_FLAG_OVERLAPPED standard handles are not supported by this synchronous
adapter; do not claim that passing a null OVERLAPPED makes them synchronous.
Normal shell/CTest redirection supplies the admitted handle mode. No runtime
async conversion or undocumented handle-mode probe is introduced.

| Operation | Contract |
|---|---|
| Append | Synchronous complete WriteFile loop, bounded DWORD requests |
| Flush | Validate open adapter; no buffering to drain |
| Sync | Validate open adapter; no durability promise for arbitrary sinks |
| Close | Mark adapter terminal; do not CloseHandle |
| destructor | Never close the caller's handle |

Append handles successful short writes, captures GetLastError before other
calls, and reports zero progress rather than looping. Capture GetLastError only when the native
API reports failure. An absent NULL standard handle and a successful
zero-progress write have deterministic Io diagnostics, not stale native
errors. GetStdHandle failure provenance is captured immediately when needed.
Validate the sink lazily on use: a command that never uses stderr need not
fail merely because stderr is absent. A closed pipe becomes a typed Io;
Windows needs no SIGPIPE handling.
Do not silently retry denied/media errors or use unchecked std::cout buffering.
The command's TrackingOutput still aborts further inputs after an output
failure. A diagnostic-output error must not be rewritten as a successful dump.

Version-1 output is ASCII byte text with literal LF newlines. WriteFile avoids
text-mode CRLF conversion, so a redirected binary file or pipe receives the
same bytes as POSIX. No console UTF-8 code-page mutation is necessary for
already escaped ASCII keys, values, paths, and messages.

## Decision 3: Select actual capabilities without changing consumers

Add native output source and Windows executable selection only on the admitted
backend. The main's POSIX signal/descriptor path remains separately selected.
Discover the admitted backend before computing the tool-default option;
WIN32 alone does not establish support.
Top-level Windows builds gain the same default tool as top-level Linux/macOS.
Subproject consumers still default tools off and acquire no executable or
Python/test dependency.

Explicit tools on unsupported platforms retain a clear configure failure.
Do not loosen extended, benchmark, profiling, or fuzz gates as a side effect.
Keep strict warnings, consumer checks, docs-only CI routing, and nonempty
test selections.

The tool performs offline copied file reads; it does not open a Database or
require namespace-durability consent for a read-only operation. This does not
certify every Windows filesystem or add network-storage database support.
Document exact executable invocation and read-only limitations in the README
and ADR-0062 current-platform summary without rewriting its original design.

## Verification and review gate

Observe Windows tools configuration fail under the present POSIX-only guard
before admission. Express native argv/output behavior with failing tests before
implementation. Required evidence:

- All existing narrow command/help/usage/output/corruption tests still pass.
- Native missing/help/option cases preserve exact return codes and reject
  invalid arguments before reads.
- Native wide paths with Chinese/non-BMP characters, spaces, long/extended
  spellings reach the production WAL/MANIFEST/SST decoders correctly.
- The real executable emits exact version-1 ASCII/LF bytes through redirected
  files and pipes, including escaped Unicode display.
- Input errors continue to later inputs, stderr failure stops, and stdout
  failure prevents later reads; neither becomes success.
- Native short/zero writes and handle failures use the private WriteFile seam;
  real closed pipes demonstrate actual kernel error behavior.
- Adapter Close/destruction do not close the borrowed handle, and terminal
  adapter operations are rejected.
- Filenames and exact input bytes/digests before/after native dump remain
  unchanged. Stdout/stderr capture artifacts stay outside the fixture directory.
  Fixtures come from closed databases; no live-database/backup claim is inferred.
- Raw-byte CLI capture verifies exact exit codes and LF output without newline
  normalization; WILL_FAIL alone is not exit-code evidence. Closed-pipe tests
  close every reader copy before writing, and stale last-error tests cover
  absent handles and zero progress.
- MSVC Debug/Release, real CLI help/usage contracts, selected dump/output tests,
  consumer configuration, and existing POSIX gates remain green.
- Independent diagnostic/storage-contract and Win32/Unicode/output/build
  personas review the exact implementation and focused fixes before merge.

Reuse the existing owned Windows test-process fixture or normal bounded
subprocess test tooling only where it fits. Do not grow a collector/process
framework just to capture a small command's stdout.

## Design review record

Independent diagnostic/storage-contract and Win32/Unicode/output/build
personas reviewed the design. Three native-output gaps were accepted:
synchronous-handle preconditions, deterministic absent/zero-progress error
provenance with lazy sink use, and exact input-byte read-only evidence.
The focused closure confirmed those corrections and compatibility with
the existing POSIX, version-1 schema, aliasing, and ownership contracts.
No actionable finding remains from these bounded design passes.

ASCII English, LF/whitespace, fences, local references, DAG ordering and the
exact two-file documentation-only scope were checked. No production/runtime
validation is claimed by this design PR.

## Implementation evidence

The internal native-path entry point and existing narrow entry share one
execution implementation. Windows wmain retains native wide path identity;
the copied filesystem and borrowed synchronous sinks use native APIs.
Capability admission is computed before standalone tool defaults, and
subprojects still acquire no tool or Python dependency by default.

Windows tools configuration was observed failing under the original guard;
three native command/output cases failed against stubs before implementation.
Native sink tests then verified full short writes, deterministic zero/absent
errors despite stale last-error state, terminal markers without OS-handle
close, and actual kernel closed-pipe errors. The native pipe oracle accounts
for ERROR_BROKEN_PIPE and ERROR_NO_DATA without changing reported provenance.

The real command's raw-byte test dumps closed native WAL, MANIFEST, and SST
files under ordinary Unicode and explicit extended paths, verifies exact
0/1/2 return codes and LF bytes, compares file redirection against pipe capture,
closes all stdout readers before a failed write, and compares filenames plus
input digests before/after. Output capture stays outside the fixture directory.

MSVC /W4 /WX passed all 707 Debug unit/consumer/tool cases and 690 Release
unit/tool cases. A fresh admitted Windows standalone configuration also
verified the tool defaults ON without relying on a previous option cache.
These are offline decoder/output checks, not online backup or whole-file
corruption certification.

Independent diagnostic-contract and native-platform code reviews closed
without remaining actionable issues. The platform reviewer identified a
closed-reader launch race in the raw CLI test: the pipe reader now closes
before launch, with only the writer passed and owned timeout cleanup checked.
The project-test opt-out now also gates fixture/Python acquisition, verified
by a consumer configuration that failed before that gate repair.

## References

- [ADR-0006 review gates](0006-sequential-pull-request-workflow.md)
- [ADR-0062 diagnostic contracts](0062-read-only-storage-file-diagnostics.md)
- [ADR-0064 native filesystem](0064-windows-filesystem-and-delivery.md)
- [Current command seam](../../src/diagnostics/dump_command.h)
- [Current POSIX sink](../../src/diagnostics/posix_output.cc)
- [GetStdHandle](https://learn.microsoft.com/en-us/windows/console/getstdhandle)
- [WriteFile](https://learn.microsoft.com/en-us/windows/win32/api/fileapi/nf-fileapi-writefile)
