# ADR-0027: Versions and the Version Set

- Status: Accepted
- Date: 2026-09-24

## Context

A version is the set of live table files in every level. The version set
owns the current version and the database counters, and persists every change
as a version edit ([ADR-0021](0021-manifest-version-edits.md)) in the MANIFEST,
a WAL-framed log ([ADR-0018](0018-wal-stream-io.md)) that `CURRENT` names
([ADR-0020](0020-database-file-names.md)). Recovery replays the MANIFEST;
flushes and compactions append edits.

This implements only the `implement-version-set` DAG node: building versions
from edits, and creating, recovering, and appending to the MANIFEST. Reading
tables through versions, choosing compactions, replaying WALs, and deleting
obsolete files belong to the read-path, compaction, recovery, and engine
nodes.

## Current Modern LevelDB callers

| Future caller | Required behavior |
|---|---|
| Database opening | Create the MANIFEST of a new database, or recover the current version and counters of an existing one |
| Recovery | Read the log numbers and the last sequence, mark replayed WAL numbers used, raise the last sequence, and record the new WAL and any flushed tables |
| Write path | Read and raise the last sequence, and allocate WAL numbers |
| Read path | Take the current version and read its files by level |
| Flush | Allocate table numbers and record new tables and the new log number |
| Compaction | Read files by level and compact pointers, and record removed inputs, new outputs, and compact pointers |
| Obsolete-file cleanup | Read the MANIFEST number, the log numbers, and the files of every version still in use |

No current caller requires rotating the MANIFEST while the database is open,
reusing the recovered MANIFEST, compaction scores, seek statistics, or
releasing the database lock while the MANIFEST is written.

## Prior art

### Google LevelDB

Adopt LevelDB's version set:

- A version lists every level's files by smallest internal key, with the file
  number breaking ties. Files in levels 1 and above do not overlap.
- A builder applies edits to a base version: deletions first, then additions,
  so one edit can move a file between levels.
- Recovery reads `CURRENT`, replays every MANIFEST record into one builder
  over an empty version, requires the next file number, log number, and last
  sequence, defaults the previous log number to zero, and rejects a MANIFEST
  whose comparator name differs with `InvalidArgument`. Any corruption
  reported while reading the MANIFEST fails recovery; a tail that a crash cut
  short is ignored. The next MANIFEST is numbered from the recovered next file
  number, and allocation continues after it.
- `LogAndApply` fills the edit's log number and previous log number when they
  are absent and always sets the next file number and last sequence. The
  first edit after opening writes a new MANIFEST that starts with a snapshot
  of the current version (comparator name, compact pointers, and every file)
  followed by the edit, syncs it, and installs it by writing `CURRENT` through
  a temporary file and a rename. Later edits are appended and synced.
- A new database's MANIFEST records the comparator name, log number 0, next
  file number 2, and last sequence 0 in `MANIFEST-000001`.

Change:

- **Validate edits.** LevelDB only asserts, in debug builds, that files in
  levels 1 and above do not overlap, silently ignores deleting a file that is
  not live, and does not check counters. Here the builder rejects deleting a
  file that is not live, adding a file number that is already live, a file
  whose smallest key follows its largest, and overlapping files in levels 1
  and above. `LogAndApply` also rejects log numbers and new file numbers at
  or above the next file number. Recovery rejects a MANIFEST whose log number
  decreases between records or that names a log or file number, in any
  record, that is not below its final next file number. The same rules
  apply to edits being written and records being recovered, so a MANIFEST
  that `LogAndApply` wrote is always recoverable.
- **Never replace the named MANIFEST.** LevelDB numbers the next MANIFEST
  with the recovered next file number. Repairing an empty database records
  next file number 1 in `MANIFEST-000001`, so LevelDB's first edit truncates
  the MANIFEST that `CURRENT` names. Here the next MANIFEST's number is also
  above the recovered MANIFEST's.
- **Bounded file numbers.** File numbers stay below 2^63: larger numbers from
  `CURRENT`, the MANIFEST, or a file name are rejected, and `LogAndApply`
  rejects edits once allocation passes the bound, so every MANIFEST the
  version set writes stays recoverable and allocation can never overflow. No
  real database comes near the bound.
- **Durable installation.** LevelDB syncs neither the new MANIFEST's directory
  entry nor the rename of `CURRENT`. Here the directory is synced before
  `CURRENT` can name the new MANIFEST and again after the rename, as
  [ADR-0011](0011-filesystem-contracts.md) requires.
- **One failure stops edits.** After a failed MANIFEST write, LevelDB keeps
  the MANIFEST open, and its database stops writing edits because it records
  a background error. Here any I/O failure in `LogAndApply` makes the version
  set reject all later edits with that error, because the edit may or may not
  survive a crash.
- **Shared file metadata.** LevelDB reference-counts file metadata and
  versions by hand and links every version into a list to find the files that
  older versions still use. Here versions share
  `std::shared_ptr<const FileMetadata>`, callers hold versions through
  `std::shared_ptr<const Version>`, and the version set tracks the versions it
  installs with `std::weak_ptr`.
- **Creation through the version set.** LevelDB's database writes the initial
  MANIFEST itself and then recovers it. Here `VersionSet::Create` writes the
  same state through `LogAndApply` and returns the open version set.

### RocksDB

RocksDB's version set adds column families, atomic groups of edits, batched
MANIFEST writes, MANIFEST rotation at a size limit, best-effort recovery, and
a new MANIFEST after a failed write so that later edits can still commit. Its
version builder also reports inconsistent edits as corruption. The extensions
are rejected because no current caller needs them; the engine stops writing
after a failed edit, as LevelDB's does.

## Decision

Add `src/metadata/version.{h,cc}` and `src/metadata/version_set.{h,cc}`:

```cpp
class Version final {
 public:
  using File = std::shared_ptr<const FileMetadata>;

  Version() = default;
  std::span<const File> files(std::uint32_t level) const noexcept;
};

class VersionBuilder final {
 public:
  VersionBuilder(const InternalKeyComparator& comparator, const Version& base);
  VersionBuilder(const InternalKeyComparator&& comparator, const Version& base) = delete;

  Status Apply(const VersionEdit& edit);
  Result<Version> Build() const;
};

class VersionSet final {
 public:
  static Result<std::unique_ptr<VersionSet>> Create(FileSystem& file_system,
                                                    std::filesystem::path directory,
                                                    const InternalKeyComparator& comparator);
  static Result<std::unique_ptr<VersionSet>> Recover(FileSystem& file_system,
                                                     std::filesystem::path directory,
                                                     const InternalKeyComparator& comparator);
  // Both factories also have deleted overloads for temporary comparators.

  std::shared_ptr<const Version> current() const noexcept;
  std::span<const std::optional<InternalKey>, NumLevels> compact_pointers() const noexcept;
  std::set<std::uint64_t> LiveFiles() const;

  std::uint64_t manifest_file_number() const noexcept;
  std::uint64_t NewFileNumber() noexcept;
  Status MarkFileNumberUsed(std::uint64_t number);
  SequenceNumber last_sequence() const noexcept;
  void SetLastSequence(SequenceNumber sequence) noexcept;
  std::uint64_t log_number() const noexcept;
  std::uint64_t prev_log_number() const noexcept;

  Status LogAndApply(VersionEdit edit);
};

inline constexpr std::uint64_t FileNumberLimit = std::uint64_t{1} << 63U;
```

### Versions and the builder

- A `Version` is immutable. A default-constructed version has no files.
  `files(level)` requires a level below `NumLevels`.
- `Apply` removes the edit's deleted files, then adds its new files. It
  returns `InvalidArgument` for a deleted file that is not live in that level,
  a new file whose number is live in any level, or a new file whose smallest
  key follows its largest. After an error, the builder may only be destroyed.
- `Build` returns the files of every level sorted by smallest key and file
  number, or `InvalidArgument` if two files in a level above 0 overlap: each
  file's largest key must precede the next file's smallest key.
- Compact pointers are not part of a version; the version set keeps the
  latest pointer of each level.

### Recovery

`Recover` reads `CURRENT` and replays the MANIFEST it names:

- A missing `CURRENT` is `NotFound`, which tells the caller that no database
  exists. Invalid `CURRENT` contents and a MANIFEST that `CURRENT` names but
  that does not exist are `Corruption`.
- A corruption event from the WAL reader, a record that does not decode, a
  record the builder rejects, a MANIFEST without a next file number, log
  number, or last sequence, a log number that decreases between records, a
  log number, previous log number, or file number in any record that is not
  below the final next file number, a next file number above
  `FileNumberLimit`, and a MANIFEST number at or above it are `Corruption`. A
  comparator name that differs from the user comparator's is
  `InvalidArgument`. File read errors are returned unchanged.
- The next MANIFEST's number is the recovered next file number, or the
  recovered MANIFEST's number plus one if that is larger, and `NewFileNumber`
  continues after it. The recovered MANIFEST is not reused or modified: the
  first `LogAndApply` writes a new one.

### Recording edits

`LogAndApply` returns `InvalidArgument` and writes nothing once allocation
has passed `FileNumberLimit`, or if the edit names a different comparator, if
its log number is below the current one or not below the next file number,
if its previous log number is not below the next file number, if it adds a
file whose number is not below the next file number, or if the builder
rejects it. A file that moves between levels keeps its recovered number,
which is below the next file number. Otherwise `LogAndApply` fills the log
numbers when absent, sets the next file number and last sequence, and writes
the edit:

```text
first edit of a version set (the edit Create writes, or the first after Recover):
  open MANIFEST-<n> → append snapshot → append edit → Sync
  → SyncDirectory(database)
  → open <n>.dbtmp → append "MANIFEST-<n>\n" → Sync → Close
  → Rename <n>.dbtmp to CURRENT → SyncDirectory(database)

later edits:
  append edit → Sync
```

The snapshot is the current version before the edit, with the comparator
name and the compact pointers. On success the new version, compact pointers,
and log numbers become current. Files that an edit adds must already be
durable, as ADR-0011 requires of the caller.

If any file operation fails, the version set keeps its previous state and
returns that error from this and every later `LogAndApply`. It removes
nothing: before the rename, `CURRENT` still names the previous MANIFEST, and
after it, the new one, so reopening recovers the previous or the new state and
never finds `CURRENT` naming a missing MANIFEST. A record cut short by a failed
append is a truncated tail that recovery ignores. The edit may or may not
survive a crash, so the database must be reopened, and files that the edit
adds must not be deleted before then. Files that the failure leaves behind are
obsolete after reopening.

### Counters

- `Create` writes `MANIFEST-000001` with log number 0, previous log number 0,
  next file number 2, and last sequence 0, installs `CURRENT`, and returns a
  version set that appends later edits to that MANIFEST. The directory must
  exist and must not contain a database.
- `NewFileNumber` returns the next file number and advances it.
  `MarkFileNumberUsed` advances the next file number past a number found on
  disk, such as a WAL that recovery replays, and returns `InvalidArgument`
  for a number at or above `FileNumberLimit`. A MANIFEST may record
  `FileNumberLimit` as its next file number; once allocation passes it, every
  edit is rejected, so the version set never writes a next file number that
  recovery rejects. Wrapping past `UINT64_MAX` would take 2^63 further
  allocations, so allocation never overflows.
- `SetLastSequence` requires a sequence at least the current one and at most
  `MaxSequenceNumber`. Its callers pass sequences of write batches, which are
  validated when decoded, or ones they allocate.
- `LiveFiles` returns the numbers of the files in every installed version that
  is still held, including the current one.
- The version set is neither copyable nor movable, and its methods require
  external synchronization. Versions and file metadata are immutable, so a
  version from `current()` may be read concurrently. The file system and the
  comparator must outlive the version set.

## Explicitly deferred behavior

- Lookups, iterators, and overlap queries over versions, which belong to the
  read-path, flush, and compaction nodes.
- Compaction scores, seek statistics, and choosing compactions.
- Releasing the database lock while the MANIFEST is written.
- MANIFEST rotation, reuse of the recovered MANIFEST, and new MANIFESTs after
  failed writes.

## Validation plan

Unit tests cover:

- Builder additions, deletions, moves between levels, ordering, and every
  rejection, including files that share a boundary user key.
- `Create`, `Recover`, and `LogAndApply` round trips through an in-memory file
  system, including snapshots, compact pointers, counters, and repeated
  reopening against an ordered model of random edits.
- The file operations and their order for the first and later edits.
- A failure at every file operation of `LogAndApply`, leaving the previous
  state, rejecting later edits, removing nothing, and recovering, after
  reopening, the previous state before the rename of `CURRENT` or before a
  complete record, and the new state after it.
- Every `Recover` rejection, a MANIFEST whose last record a crash cut short,
  and a repaired MANIFEST whose next file number does not exceed its own
  number.
- `LiveFiles` across held and released versions, and the file-number bounds.
- Every line and branch of the new code, as required by
  [ADR-0019](0019-test-coverage-policy.md).

A session-only differential helper recovers MANIFESTs that unmodified
LevelDB's version set wrote from random edits, and has LevelDB recover
MANIFESTs that this version set wrote, comparing files, compact pointers, and
counters.

## Consequences

- Every MANIFEST that the version set writes is recoverable by LevelDB and by
  the version set, and LevelDB MANIFESTs are recoverable unless they violate
  the invariants above.
- A failed MANIFEST write stops later edits until the database is reopened.
- A MANIFEST grows until the database is reopened.

## References

- [Google LevelDB version set](https://github.com/google/leveldb/blob/7ee830d02b623e8ffe0b95d59a74db1e58da04c5/db/version_set.cc)
- [Google LevelDB `SetCurrentFile`](https://github.com/google/leveldb/blob/7ee830d02b623e8ffe0b95d59a74db1e58da04c5/db/filename.cc)
- [Google LevelDB `NewDB`](https://github.com/google/leveldb/blob/7ee830d02b623e8ffe0b95d59a74db1e58da04c5/db/db_impl.cc)
- [RocksDB version set](https://github.com/facebook/rocksdb/blob/main/db/version_set.cc)
- [RocksDB version builder](https://github.com/facebook/rocksdb/blob/main/db/version_builder.cc)
