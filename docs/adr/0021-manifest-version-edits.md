# ADR-0021: MANIFEST Version Edits

- Status: Accepted
- Date: 2026-09-23

## Context

The MANIFEST is a WAL-framed log of version edits. Each edit records changes
to the set of live table files and to database-wide counters. Recovery replays
the edits to rebuild the current version. Flush, compaction, and database
opening append new edits.

This implements only the `implement-version-edit` DAG node: the persistent
edit representation, its encoder, and its decoder. Applying edits to versions,
the version set, MANIFEST file management, and runtime per-file state remain
later nodes.

## Current Modern LevelDB callers

| Future caller | Required behavior |
|---|---|
| VersionSet log-and-apply | Inspect optional counters, fill missing ones, and encode the edit |
| VersionSet recovery | Decode edits, compare the comparator name, and read optional counters |
| Version builder | Iterate compact pointers, deleted files, and new files |
| VersionSet snapshot | Record the comparator name, compact pointers, and every live file |
| Flush and recovery | Add level-0 or deeper table files |
| Compaction | Remove input files, add output files, and record compact pointers |
| Database opening | Set log numbers after WAL recovery |

No current caller requires debug strings, clearing an edit for reuse, runtime
file state such as reference counts or seek budgets, or RocksDB extensions
such as column families, blob files, file checksums, or epoch numbers.

## Prior art

### Google LevelDB

Adopt the persistent format exactly. An edit is a sequence of fields, each
introduced by a varint32 tag:

| Tag | Field | Payload |
|---|---|---|
| 1 | Comparator name | length-prefixed bytes |
| 2 | Log number | varint64 |
| 3 | Next file number | varint64 |
| 4 | Last sequence | varint64 |
| 5 | Compact pointer | varint32 level, length-prefixed internal key |
| 6 | Deleted file | varint32 level, varint64 file number |
| 7 | New file | varint32 level, varint64 number, varint64 size, length-prefixed smallest and largest internal keys |
| 9 | Previous log number | varint64 |

Tag 8, historically used for large value references, is not supported.

The encoder writes the fields in LevelDB's order: comparator, log number,
previous log number, next file number, last sequence, compact pointers in
insertion order, deleted files sorted by level and number, and new files in
insertion order. Deleted files form a set, so removing the same file twice
records it once.

The decoder reads fields until the input ends. A repeated scalar field keeps
its last value, and levels must be below seven.

Change:

- Validate values on mutation. Levels must be below seven, table file numbers
  and the next file number must be nonzero, the last sequence must fit the
  56-bit sequence range, and length-prefixed fields must fit varint32 lengths.
  An invalid mutation returns `InvalidArgument` and leaves the edit unchanged,
  so every edit is encodable.
- Decode strictly. Beyond LevelDB's checks, zero table file numbers, a zero
  next file number, last sequences above `MaxSequenceNumber`, and malformed
  internal keys are `Corruption`. LevelDB never writes such values: file
  numbers start at one, and the next file number starts at two. Accepting them
  would violate the file-name and internal-key invariants that later readers
  rely on; recovery, for example, names the next MANIFEST after the next file
  number.
- Report every decoding failure as `Corruption` naming the offending field,
  and return no partially decoded edit.
- Store optional counters as `std::optional` values instead of flag and value
  pairs.

### RocksDB

RocksDB keeps LevelDB's tags and adds many more for column families, WAL
tracking, blob files, checksums, temperatures, and timestamps, with a
forward-compatibility bit for unknown tags. These are rejected because no
current caller needs them.

## Decision

Add to the `metadata` layer:

```cpp
inline constexpr std::uint32_t NumLevels = 7;

struct FileMetadata {
  std::uint64_t number;
  std::uint64_t file_size;
  InternalKey smallest;
  InternalKey largest;
};

struct CompactPointer {
  std::uint32_t level;
  InternalKey key;
};

struct DeletedFile {
  std::uint32_t level;
  std::uint64_t number;
  friend auto operator<=>(const DeletedFile&, const DeletedFile&) = default;
};

struct NewFile {
  std::uint32_t level;
  FileMetadata file;
};

class VersionEdit {
 public:
  Status SetComparatorName(std::string_view name);
  void SetLogNumber(std::uint64_t number) noexcept;
  void SetPrevLogNumber(std::uint64_t number) noexcept;
  Status SetNextFileNumber(std::uint64_t number);
  Status SetLastSequence(SequenceNumber sequence);
  Status AddCompactPointer(std::uint32_t level, InternalKey key);
  Status AddFile(std::uint32_t level, FileMetadata file);
  Status RemoveFile(std::uint32_t level, std::uint64_t number);

  const std::optional<std::string>& comparator_name() const noexcept;
  std::optional<std::uint64_t> log_number() const noexcept;
  std::optional<std::uint64_t> prev_log_number() const noexcept;
  std::optional<std::uint64_t> next_file_number() const noexcept;
  std::optional<SequenceNumber> last_sequence() const noexcept;
  std::span<const CompactPointer> compact_pointers() const noexcept;
  const std::set<DeletedFile>& deleted_files() const noexcept;
  std::span<const NewFile> new_files() const noexcept;

  std::vector<std::byte> Encode() const;
  static Result<VersionEdit> Decode(ByteView encoded);
};
```

`VersionEdit` is a copyable, movable value. `FileMetadata` holds only
persistent fields; the version set adds runtime state separately.

### Mutation

- Scalar setters record the value and replace any earlier one.
- Log numbers may be zero, which LevelDB uses as the "no log" sentinel.
- `SetNextFileNumber` rejects zero.
- `SetLastSequence` rejects values above `MaxSequenceNumber`.
- `AddCompactPointer`, `AddFile`, and `RemoveFile` reject levels of seven or
  more. `AddFile` and `RemoveFile` also reject file number zero, which the
  file-name generators forbid.
- `AddCompactPointer` and `AddFile` reject keys that are not valid internal
  keys. A moved-from `InternalKey` has an empty encoding, and writing it would
  produce a record that decoding rejects.
- `SetComparatorName`, `AddCompactPointer`, and `AddFile` reject a comparator
  name or encoded internal key longer than `UINT32_MAX` bytes, because its
  length must fit varint32. Only inputs larger than 4 GiB reach these guards,
  so their branches are excluded from coverage under
  [ADR-0019](0019-test-coverage-policy.md).
- A rejected mutation returns `InvalidArgument` and changes nothing. A name
  that views the current comparator name is valid input.
- File metadata is otherwise recorded as given. Consistency between files,
  such as `smallest <= largest`, belongs to the version builder.

### Encoding

`Encode` returns the exact LevelDB byte sequence for the recorded fields. It
cannot fail because mutation already guarantees that every length fits
varint32. An edit with no fields encodes to an empty byte sequence.

### Decoding

`Decode` accepts any sequence of the fields above, including the empty
sequence, and returns an edit that re-encodes to the canonical order. It
returns `Corruption` for:

- a truncated tag or payload;
- an unknown tag, including tag 8;
- a level of seven or more;
- a file number of zero in a deleted-file or new-file field;
- a next file number of zero;
- a last sequence above `MaxSequenceNumber`;
- an internal key that `InternalKey::Decode` rejects.

## Explicitly deferred behavior

- Applying edits, version construction, and consistency checks such as
  overlapping level files or `smallest <= largest`.
- Runtime file state such as reference counts and seek budgets.
- Clearing an edit for reuse and debug strings.
- MANIFEST writing, rotation, and `CURRENT` installation.
- RocksDB extension tags and forward-compatible unknown tags.

## Validation plan

Unit tests cover:

- Exact golden bytes for an edit containing every field.
- Round trips, including LevelDB's large-number test pattern.
- The empty edit.
- Sorting and deduplication of deleted files and the insertion order of other
  repeated fields.
- Last-value-wins decoding of repeated scalar fields.
- Every mutation rejection except the length guards, with the edit left
  unchanged.
- Every truncation prefix of a complete edit: each prefix either decodes to an
  edit that re-encodes to exactly that prefix or returns `Corruption`.
- Every decoding rejection listed above.
- Every line and branch of the new code, as required by
  [ADR-0019](0019-test-coverage-policy.md).

A session-only differential helper compares encodings of randomized edits with
unmodified LevelDB in both directions.

## Consequences

- MANIFEST records remain byte-compatible with LevelDB for every value
  LevelDB can produce.
- Recovery rejects corrupt edits before they can create invalid versions or
  file names.
- Adding a new edit field requires a new tag and an ADR update.

## References

- [Google LevelDB version edit](https://github.com/google/leveldb/blob/7ee830d02b623e8ffe0b95d59a74db1e58da04c5/db/version_edit.h)
- [Google LevelDB version edit implementation](https://github.com/google/leveldb/blob/7ee830d02b623e8ffe0b95d59a74db1e58da04c5/db/version_edit.cc)
- [Google LevelDB version edit tests](https://github.com/google/leveldb/blob/7ee830d02b623e8ffe0b95d59a74db1e58da04c5/db/version_edit_test.cc)
- [RocksDB version edit](https://github.com/facebook/rocksdb/blob/main/db/version_edit.h)
