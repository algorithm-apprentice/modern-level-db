# ADR-0062: Read-Only Storage File Diagnostics

- Status: Implemented

## Status

Implemented by PR #94 after design PR #93.

Implementation and final validation are complete. A
bounded GPT-5.6 Sol implementation review found one corruption-precedence
issue; a regression now preserves an earlier malformed internal-key error
when a later reachable table block is also corrupt.

[ADR-0066](0066-native-windows-diagnostic-command.md) extends the command to
admitted native Windows builds with wide argv and borrowed synchronous output
handles. Its output schema, offline/read-only limits, and corruption behavior
remain unchanged; POSIX-only command descriptions below are historical scope.

## Context

Modern LevelDB already owns strict decoders for its LevelDB-compatible
persistent formats:

- WAL physical records and logical-record reassembly;
- Put/Delete write batches;
- MANIFEST `VersionEdit` records;
- internal keys;
- stored SSTable blocks, compression, indexes, and forward iteration; and
- canonical database file names.

Those decoders are exercised by unit, compatibility, crash, fuzz, and
cross-open tests, but a person studying or diagnosing a database cannot invoke
them from a command line. The only current way to understand a file is to
write a test or read raw hex.

Pinned Google LevelDB revision
`7ee830d02b623e8ffe0b95d59a74db1e58da04c5` provides:

```cpp
Status DumpFile(Env* env, const std::string& fname, WritableFile* dst);
```

and:

```text
leveldbutil dump files...
```

Its utility recognizes log, descriptor, and table file names and prints their
logical contents. It is a diagnostic decoder, not an export format, backup,
checkpoint, repair operation, or inverse of a restore command.

The user has now identified the same concrete learning and diagnosis need for
Modern LevelDB. This ADR adds the smallest complete read-only tool without
making diagnostic formatting a public database API.

## Decision

Add an internal diagnostic operation:

```cpp
// src/diagnostics/dump_file.h
[[nodiscard]] Status DumpFile(FileSystem& file_system,
                              const std::filesystem::path& path,
                              WritableFile& output);
```

Add a standalone POSIX command:

```text
modern_leveldb_tool dump FILE...
```

The command calls the internal operation for each file in argument order.
It continues to later files after one fails and exits zero only when every
file is read, decoded, and written without an error.

No public header under `include/modern_leveldb/` is added. Applications that
need database operations continue to use `Database`; this tool is a
repository diagnostic and learning surface.

## Supported file types

The file's final path component must be a canonical name accepted by
`ParseFileName`.

| File type | Example | Decoder |
|---|---|---|
| WAL log | `000123.log` | `WalReader`, then `WriteBatchReader` |
| MANIFEST | `MANIFEST-000123` | `WalReader`, then `VersionEdit::Decode` |
| SSTable | `000123.ldb` | `Table::Open` and forward `Table::Iterator` |

`CURRENT`, `LOCK`, `.dbtmp`, noncanonical names, and unknown suffixes return
`InvalidArgument`. A canonical-looking directory or other non-regular path
propagates the filesystem's size/open/read result because `FileSystem` has no
portable file-kind query. Legacy `.sst` aliases are not added because
Modern's canonical filename contract deliberately recognizes only names it
generates.

The operation dispatches from the filename, as pinned LevelDB does. It does
not guess a format from arbitrary bytes or retry another decoder after one
fails.

## Read-only and consistency contract

`DumpFile` performs only:

- filename parsing;
- file-size lookup for tables;
- sequential or random-access file opening;
- reads through existing format decoders; and
- `output.Append` calls.

It never:

- opens a `Database`;
- acquires or modifies the database lock;
- creates, appends, renames, syncs, removes, truncates, repairs, or rewrites a
  database file;
- changes cache or compaction state; or
- calls `Flush`, `Sync`, or `Close` on the caller-owned output.

The input must be from a closed database, a stable frozen fixture, or an
already consistent offline copy. Reading individual files while another
process appends a WAL or MANIFEST or installs/removes files can produce a
mixed-time diagnostic view. The tool does not claim to create an online
backup or checkpoint.

The caller-provided `output` must not alias `path` or any database file.
`WritableFile` exposes no path identity, so `DumpFile` cannot enforce this
precondition. The command-line tool writes only to stdout, but the caller must
not redirect stdout onto an input or other database file.

The output contains user keys and values and may expose sensitive application
data. It is sent only to the caller-provided sink; the caller controls
redirection, persistence, and any later transmission.

## Text format

The output is deterministic ASCII with one newline-terminated item per
`Append` call. Every dump starts with:

```text
dump version=1 type=<log|manifest|table> file='<escaped path>'
```

Version 1 is a diagnostic text schema, not a persistent database format.
Future incompatible rendering changes require another version.

All untrusted bytes are single-quoted and escaped:

- printable ASCII bytes other than `'` and `\` appear directly;
- `'` becomes `\'`;
- `\` becomes `\\`;
- newline, carriage return, and tab become `\n`, `\r`, and `\t`; and
- every other byte becomes lowercase `\xNN`.

No raw control byte from a key, value, comparator name, path, or error message
is written to the diagnostic stream.

Paths use `std::filesystem::path::generic_u8string()`. Its UTF-8 code units
are treated as bytes and escaped by the same rules, so path separators and
text are rendered consistently across platforms. A path conversion exception
returns `InvalidArgument` before input is opened or output is written. CLI
stderr uses the same projection; if conversion itself fails, it uses the
literal `<unrepresentable>` path marker.

Every `error='...'` field escapes `Error::ToString()`, including the stable
error-code name and message.

### WAL records

A valid batch is rendered as:

```text
record offset=0 sequence=40 count=2
  put sequence=40 key='color' value='red'
  delete sequence=41 key='old'
```

`offset` is the logical record's physical starting offset reported by
`WalReader`. Each operation includes its assigned sequence.

A logical WAL record that is not a valid complete write batch is rendered:

```text
record_error offset=32768 error='corruption: ...'
```

### MANIFEST records

Each record starts with:

```text
record offset=0
```

Present scalar and repeated fields then use one indented line each:

```text
  comparator name='leveldb.BytewiseComparator'
  log_number value=2
  previous_log_number value=0
  next_file_number value=7
  last_sequence value=41
  compact_pointer level=1 key={user_key='m' sequence=72057594037927935 kind=value}
  delete_file level=0 number=3
  add_file level=1 number=5 size=4096 smallest={...} largest={...}
```

Fields are printed in the same category order as `VersionEdit::Encode`:
comparator, log number, previous log number, next file number, last sequence,
compact pointers, deleted files, and new files. Repeated scalar fields have
already been reduced by decoding, so only their last value is printed.
Compact pointers and new files retain decoded insertion order. Deleted files
are printed once in the `std::set`'s canonical `(level, number)` order; their
wire order and duplicates are intentionally not retained by `VersionEdit`.

An invalid logical record uses `record_error`.

### SSTable entries

Forward physical iteration renders:

```text
entry user_key='color' sequence=41 kind=value value='blue'
entry user_key='color' sequence=40 kind=deletion value=''
```

An entry whose key is not a valid internal key is rendered:

```text
bad_key encoded='\x01...' value='...'
```

and is treated as corruption.

The `value` field is always printed, including for deletions, so an unusual
nonempty deletion payload is not hidden.

The dump opens tables with:

- the bytewise user comparator;
- no filter policy;
- no block cache;
- copied reads in the command-line filesystem; and
- defensive rather than trusted internal-key comparison.

This is valid even for a table built with a custom user comparator because
the diagnostic path uses only `SeekToFirst` and `Next`. It never calls
`Seek`, `SeekToLast`, or `Prev`, and Modern's table-open index validation
checks physical entry/handle validity without comparing key order.

The table path is a reachable logical-entry diagnostic, not a whole-file
verifier. With no filter policy, `Table::Open` does not read the metaindex or
filter block. Defensive index validation checks entry and block-handle
structure but does not parse or order-check index keys. Unreferenced bytes
and blocks are not visited. Success means the selected open and forward
traversal encountered no error; it does not certify that every file byte is
valid.

## Corruption and error behavior

The tool must surface corruption encountered by the selected decoder or
reachable traversal without discarding later diagnostic evidence that can
still be read safely.

### WAL-framed files

For every `WalCorruption` event, append:

```text
corruption offset=123 dropped_bytes=456 error='corruption: ...'
```

Remember the first corruption and continue with the next event according to
`WalReader`'s recovery boundary. At EOF, return that first corruption.

Likewise, remember and print the first invalid write-batch or VersionEdit
record while continuing to later logical records.

A crash-truncated final WAL fragment remains benign EOF because that is the
existing `WalReader` contract. The diagnostic does not relabel it as
corruption.

### Tables

Encountered table-open, reachable block-read, decompression, checksum, and
iterator failures return their typed errors. When an invalid internal key is
reachable, print the `bad_key` line, remember corruption, continue forward
when safe, and return the first corruption after iteration.

### I/O and output failures

An input I/O error or output `Append` failure stops that file immediately and
is returned unchanged. It takes precedence over an earlier remembered format
corruption because the diagnostic stream itself could not be completed.

The command writes a concise
`file='<escaped path>' error='<escaped error>'` line to stderr for every
failed file, using the same byte escaping as the dump stream. It does not
return success merely because partial text was produced.

The stdout adapter does not use the buffered database `PosixWritableFile`.
Each `Append` loops on POSIX `write`, advances after short writes, retries
`EINTR`, and reports all other failures as `Io`. The command ignores
`SIGPIPE` so a closed pipe produces `EPIPE` rather than terminating before
the error can be reported. A stdout failure stops the entire command; later
input files are not processed because the diagnostic stream is unusable.

Exceptions are not used for normal format or I/O control flow. Allocation
failure follows the project's ordinary C++ behavior.

## Ownership and layering

Add:

```text
src/diagnostics/dump_file.h
src/diagnostics/dump_file.cc
src/diagnostics/dump_command.h
src/diagnostics/dump_command.cc
src/diagnostics/posix_output.h
src/diagnostics/posix_output.cc
tools/modern_leveldb_tool.cc
tools/CMakeLists.txt
tests/unit/diagnostics/dump_file_test.cc
```

`diagnostics` is a top internal layer. It may depend on `base`, `platform`,
`format`, `wal`, `table`, and `metadata`. No lower layer depends on it, and it
does not depend on `engine` or the public `api`.

`DumpFile` borrows `FileSystem` and `WritableFile` for the call. It owns all
temporary readers, tables, iterators, and formatting buffers. The caller owns
the output and decides its destination and lifetime.

The implementation source is part of the static library so unit tests and the
tool reuse exactly one diagnostic path. Its header remains private under
`src/`.

The implementation also adds recorded, failure-injectable `FileSize` support
to `tests/support/MemoryFileSystem`. This is test infrastructure required for
portable SSTable dump tests, not a production filesystem change.

## Build contract

Add:

```cmake
option(MODERN_LEVELDB_BUILD_TOOLS
       "Build Modern LevelDB command-line tools"
       <standalone POSIX default>)
```

The default is:

- `ON` for a top-level Linux/macOS build;
- `OFF` when consumed through `add_subdirectory`; and
- `OFF` on unsupported platforms.

Explicitly enabling tools without the POSIX backend fails configuration with
a clear message. The portable diagnostic library and its in-memory unit tests
still compile on every existing test platform.

The executable target is not installed in this milestone because the project
has no installation/package rules yet. It is produced at:

```text
build/<preset>/tools/modern_leveldb_tool
```

Existing consumers do not gain a new default executable or public dependency.

## Command-line contract

Accepted forms:

```text
modern_leveldb_tool dump FILE...
modern_leveldb_tool --help
```

No file argument, an unknown command, or an unknown flag prints usage to
stderr and exits with status 2. `--help` prints usage to stdout and exits zero.

For `dump`, each path is processed in order. The core versioned header already
identifies the path, so the command adds no second ad hoc separator.

An input-file error is reported and processing continues with the next path.
A stdout error stops immediately, as described above.

The first version has no output file option, JSON mode, key filtering,
truncation limit, comparator plugin, directory recursion, repair mode, or
restore command.

## Testing plan

Development remains test-first.

### Escaping and sink behavior

- Empty, printable, quote, backslash, control, high-bit, and embedded-zero
  bytes have exact golden text.
- Every emitted item ends in one newline and uses one `Append`.
- An injected output failure stops immediately and returns that error.
- A POSIX pipe with its read end closed proves that the stdout adapter converts
  `EPIPE` into `Io` without process termination.

### WAL

- A golden two-operation batch reports batch and per-entry sequences.
- Empty, binary, and zero-length key/value data are escaped unambiguously.
- A physical checksum failure prints a corruption event, continues at the
  next safe record, and returns corruption.
- A valid WAL record containing a malformed batch prints `record_error`,
  continues, and returns corruption.
- A truncated final fragment remains successful EOF.
- Sequential-file open/read errors propagate.

### MANIFEST

- Every `VersionEdit` field has exact deterministic text.
- Compact pointers and additions retain decoded order; deleted files render
  once in `(level, number)` order; repeated scalar fields render only their
  last decoded value.
- Invalid edits print `record_error`, continue, and return corruption.
- WAL-framing corruption follows the same event behavior as logs.

### SSTables

- Values, deletions, binary user keys, and sequence boundaries render
  correctly.
- None, Snappy, and Zstd tables dump the same logical entries.
- Forward dumping succeeds for a table written with a non-bytewise custom
  comparator.
- Encountered bad footer, reachable checksum/compression/block-entry, and
  reachable internal-key failures are explicit.
- A damaged unrequested filter block demonstrates that success is not a
  whole-file verification claim.
- No dump lookup fills a block cache.

### Dispatch and CLI

- Canonical log, MANIFEST, and table names dispatch correctly.
- CURRENT, LOCK, temporary, unknown, and noncanonical names are rejected.
- Help, usage errors, multiple files, per-file continuation, escaped stderr,
  and process exit status are deterministic. Output-append failure is covered
  at the shared diagnostic operation boundary.
- A CMake consumer build does not build tools by default.
- Explicit unsupported-platform tool configuration fails clearly.

## Sequential delivery roadmap

### PR 1: design only

- Add this ADR.
- Obtain one bounded GPT-5.6 Sol design review.
- Resolve every justified finding before merge.
- Use ADR-0048 documentation-only CI acknowledgements.

### PR 2: complete diagnostic tool

Implement the internal operation, versioned text format, POSIX command,
portable unit tests, CMake integration, architecture/dependency documentation,
README usage, and a guided learning lab in one coherent PR.

Do not begin `RepairDB`, restore, backup/checkpoint, directory recursion,
manual compaction, or approximate-size work in parallel.

If implementation review shows that safe comparator-independent forward table
dumping needs a lower-level refactor larger than this boundary, stop and amend
the design rather than adding a comparator plugin or silently dropping table
support.

## Validation gates

The implementation PR runs:

1. exact focused diagnostic and CLI tests;
2. complete Debug unit and CMake consumer tests;
3. Release with warnings as errors;
4. compatibility, model, and crash tiers;
5. ASan/UBSan and TSan;
6. bounded format and stateful fuzz smoke;
7. GCC 13 changed-code coverage;
8. benchmark and profiling contract/smoke gates without collecting a new
   performance result; and
9. one bounded GPT-5.6 Sol implementation review.

The diagnostic is outside normal database operations and makes no throughput
claim.

## Acceptance criteria

The feature is complete when:

- all three supported canonical file types produce the version-1 text above;
- no input byte reaches output unescaped;
- files whose selected traversal encounters no error return success, while
  encountered corruption produces useful partial text plus a non-success
  status;
- a truncated final WAL tail retains existing benign recovery semantics;
- custom-comparator tables can be dumped through forward-only iteration;
- no database file is modified or locked;
- no public Modern LevelDB API is added;
- consumer builds do not receive the tool by default;
- the learning documentation clearly separates dump, backup, restore, and
  repair; and
- every validation gate and bounded review passes.

## Consequences

### Positive

- Existing format knowledge becomes directly observable without a debugger.
- Corruption reports use the same production decoders as database recovery
  and reads.
- Learners can connect WAL batches, MANIFEST edits, and table history.
- The tool adds no persistent format or online-engine state.

### Negative

- Diagnostic text becomes a versioned behavior that needs tests.
- Full values can produce large output and may reveal application data.
- Dumping a changing live directory is not a consistent backup.
- The command is initially available only on supported POSIX builds.

## Rejected alternatives

### Treat dump text as an export/restore format

Rejected. It would require a canonical logical data model, comparator and
option metadata, duplicate/version handling, transactional import semantics,
and atomic destination installation. The diagnostic text is intentionally
human-readable and one-way.

### Dump an entire database directory

Rejected. Selecting a consistent live file set is a backup/checkpoint problem.
For an offline database, copying the directory already preserves the original
binary files.

### Export `DumpFile` as a public library API

Rejected for the first version. The command is the only concrete caller, and
publishing formatting callbacks or output abstractions would create a broader
contract without need.

### Use `std::ifstream` instead of `FileSystem`

Rejected. Existing file interfaces provide deterministic I/O failure tests
and keep decoding independent of host stream behavior.

### Require the database's custom comparator

Rejected. Forward physical table iteration does not compare keys. Requiring a
runtime comparator registry would add a plugin system solely for diagnostics.

### Ignore corruption and return success after printing it

Rejected. Useful partial output does not make a damaged file healthy.

### Add JSON, filtering, truncation, or directory recursion now

Rejected. There is no current caller, and each option expands the text/schema
and CLI test surface.

## References

- [Pinned LevelDB dump API](https://github.com/google/leveldb/blob/7ee830d02b623e8ffe0b95d59a74db1e58da04c5/include/leveldb/dumpfile.h)
- [Pinned LevelDB dump implementation](https://github.com/google/leveldb/blob/7ee830d02b623e8ffe0b95d59a74db1e58da04c5/db/dumpfile.cc)
- [Pinned LevelDB utility](https://github.com/google/leveldb/blob/7ee830d02b623e8ffe0b95d59a74db1e58da04c5/db/leveldbutil.cc)
- [Database file names](0020-database-file-names.md)
- [WAL stream I/O](0018-wal-stream-io.md)
- [MANIFEST version edits](0021-manifest-version-edits.md)
- [SSTable reader](0025-sstable-reader.md)
- [Compatibility and crash harness](0040-compatibility-and-crash-harness.md)
- [Need-driven simplicity](0010-need-driven-simplicity.md)
- [Learning feature comparison](../learning/11-leveldb-comparison.md)
