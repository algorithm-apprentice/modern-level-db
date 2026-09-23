# ADR-0020: Database File Names

- Status: Proposed
- Date: 2026-09-23

## Context

Every persistent file in a database directory is identified by its name.
Recovery, obsolete-file cleanup, the write path, flush, compaction, the table
cache, the version set, and database opening must agree on one deterministic
naming scheme and one parser.

This implements only the `implement-filenames` DAG node: pure file-name
generation, file-name parsing, and the contents of the `CURRENT` file. File
I/O, installing `CURRENT` (temporary write, sync, rename, and directory sync),
locking, and obsolete-file policy remain in the version-set and engine nodes.

## Current Modern LevelDB callers

| Future caller | Required behavior |
|---|---|
| Write path | Name a new write-ahead log |
| Flush and compaction | Name new table files |
| Table cache | Name the table file for a file number |
| VersionSet | Name `MANIFEST` files, `CURRENT`, and the temporary file used to install `CURRENT`; encode and validate `CURRENT` contents |
| Recovery | Classify directory entries and extract log and table numbers |
| Obsolete-file cleanup | Classify logs, tables, manifests, and temporary files |
| Database opening | Name the `LOCK` file |

No current caller requires info-log names (Modern LevelDB has no info logger),
legacy `.sst` table names (written only by LevelDB releases before 1.14), a
separate WAL directory, or RocksDB `OPTIONS`, `IDENTITY`, blob, or archive
files.

## Prior art

### Google LevelDB

Adopt the file names exactly:

| Type | Name |
|---|---|
| Write-ahead log | `<number>.log` |
| Table | `<number>.ldb` |
| Descriptor (MANIFEST) | `MANIFEST-<number>` |
| Temporary file | `<number>.dbtmp` |
| Current descriptor pointer | `CURRENT` |
| Database lock | `LOCK` |

- Numbers are formatted in decimal with at least six digits, as `%06llu`.
- Parsing accepts padded and unpadded decimal numbers up to `UINT64_MAX`, is
  locale independent, and requires an exact suffix.
- `CURRENT` contains the descriptor name followed by a newline.

Change:

- Return `std::filesystem::path` values instead of concatenated strings.
- Return `std::optional` from parsing instead of a boolean plus output
  parameters.
- Parse only canonical spellings, which are exactly the generators' output
  with a nonzero number, so every parsed name regenerates to the same path.
  LevelDB also accepts spellings it never generates, such as `100.log`,
  `0.log`, and `MANIFEST-2`. A caller that regenerates a path from such a
  parse opens a different file: `MANIFEST-2` parses as 2 and regenerates as
  `MANIFEST-000002`.
- Validate `CURRENT` strictly. LevelDB checks only for the final newline and
  then opens whatever name the file contains, including names with path
  components. Modern LevelDB requires exactly one canonical descriptor name,
  so a malformed or hostile `CURRENT` cannot redirect recovery to another
  file.

Defer:

- Legacy `.sst` table names and the table-cache fallback to them.
- `LOG` and `LOG.old` until an info logger exists. Unrecognized names are
  never classified as obsolete, so such files are left untouched.
- `SetCurrentFile`, whose I/O belongs to the version set and must follow the
  durability order in [ADR-0011](0011-filesystem-contracts.md).

### RocksDB

RocksDB extends LevelDB's scheme with `OPTIONS-<number>`, `IDENTITY`, blob
files, archived WALs, separate WAL directories, and timestamped old info logs.
These are rejected because no current caller needs them.

## Decision

Add a `metadata` module:

```cpp
enum class FileType {
  Log,
  Lock,
  Table,
  Descriptor,
  Current,
  Temp,
};

struct ParsedFileName {
  FileType type;
  std::uint64_t number;
};

std::filesystem::path LogFileName(
    const std::filesystem::path& directory, std::uint64_t number);
std::filesystem::path TableFileName(
    const std::filesystem::path& directory, std::uint64_t number);
std::filesystem::path DescriptorFileName(
    const std::filesystem::path& directory, std::uint64_t number);
std::filesystem::path TempFileName(
    const std::filesystem::path& directory, std::uint64_t number);
std::filesystem::path CurrentFileName(const std::filesystem::path& directory);
std::filesystem::path LockFileName(const std::filesystem::path& directory);

std::optional<ParsedFileName> ParseFileName(std::string_view file_name) noexcept;

std::string CurrentFileContents(std::uint64_t descriptor_number);
Result<std::uint64_t> ParseCurrentFileContents(std::string_view contents);
```

### Generation

Each generator returns `directory / name`. Numbered generators require a
nonzero number, matching LevelDB's assertion: the version set allocates file
numbers starting at one, so zero indicates a programming error. Numbers are
formatted with `std::to_chars` and zero-padded to at least six digits;
`UINT64_MAX` produces a twenty-digit name.

### Parsing

`ParseFileName` classifies one file-name component, such as an entry returned
by `FileSystem::ListDirectory`; it does not accept directory paths. It
recognizes exactly:

- `CURRENT` and `LOCK`, reported with number zero;
- `MANIFEST-<number>`;
- `<number>.log`, `<number>.ldb`, and `<number>.dbtmp`.

`<number>` must be the canonical spelling of a nonzero `uint64_t`: the value
zero-padded to exactly six digits when it is below 1,000,000, and written
without leading zeros otherwise. Parsing converts the digits with
`std::from_chars`, which is locale independent and reports overflow without
undefined behavior, and then requires the digits to equal the canonical
spelling of the value. Signs, whitespace, extra or missing leading zeros,
zero, and values above `UINT64_MAX` are rejected.

Every other name returns `nullopt`. An unrecognized name is not an error,
because database directories may contain unrelated files. Such files are
never classified as logs, tables, manifests, or temporary files, so recovery
and obsolete-file cleanup ignore them.

### `CURRENT` contents

`CurrentFileContents(number)` requires a nonzero number and returns the
descriptor name followed by one newline, for example `MANIFEST-000005\n`,
byte-for-byte compatible with LevelDB's `SetCurrentFile`.

`ParseCurrentFileContents` requires the final newline and requires the
remaining text to parse as a descriptor name. Because parsing accepts only
canonical names, valid contents equal `CurrentFileContents(number)`
byte-for-byte. The function returns the descriptor number. Any other contents,
including an empty file, a missing newline, an extra newline, a path
component, a noncanonical or zero number, or a non-descriptor name, return
`Corruption`.

## Explicitly deferred behavior

- Writing, syncing, renaming, and removing files, including `CURRENT`
  installation.
- Legacy `.sst` table names.
- Info-log names.
- Obsolete-file policy and file-number allocation.
- Windows-specific handling of non-ASCII directory entries.

## Validation plan

Unit tests cover:

- LevelDB's golden cases in canonical form, and explicit rejection of the
  noncanonical, zero, `LOG`, `LOG.old`, and `.sst` names that LevelDB
  accepts.
- LevelDB's error cases.
- Round trips for every generated name.
- Number boundaries: 1, 999,999, 1,000,000, and `UINT64_MAX`; overflow; and
  padded or unpadded spellings on both sides of the six-digit boundary.
- Exact `CURRENT` contents and every rejection path.
- Every line and branch of the new module, as required by
  [ADR-0019](0019-test-coverage-policy.md).

A session-only differential helper compares generated names and the parse
results for canonical names with unmodified LevelDB.

## Consequences

- All engine components share one naming scheme that remains compatible with
  current LevelDB databases, whose files always use canonical names.
- Every parsed name regenerates to the same path, so callers may rebuild paths
  from parsed numbers.
- Recovery cannot be redirected by a malformed `CURRENT` file.
- Databases created by LevelDB releases before 1.14 are not supported until a
  real requirement appears.

## References

- [Google LevelDB file names](https://github.com/google/leveldb/blob/7ee830d02b623e8ffe0b95d59a74db1e58da04c5/db/filename.h)
- [Google LevelDB file-name implementation](https://github.com/google/leveldb/blob/7ee830d02b623e8ffe0b95d59a74db1e58da04c5/db/filename.cc)
- [Google LevelDB file-name tests](https://github.com/google/leveldb/blob/7ee830d02b623e8ffe0b95d59a74db1e58da04c5/db/filename_test.cc)
- [RocksDB file names](https://github.com/facebook/rocksdb/blob/main/file/filename.h)
