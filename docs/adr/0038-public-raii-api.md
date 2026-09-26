# ADR-0038: Public RAII Database API

## Status

Accepted

## Context

The engine in ADR-0037 now implements opening, recovery, writes, reads,
iterators, snapshots, flushes, compactions, and shutdown. Its headers remain
under `src/`: they expose injected filesystem, executor, clock, cache, table,
and sequence-number types that are implementation details rather than a
stable application interface.

The `implement-public-api` DAG node must provide the first supported entry
point under `include/modern_leveldb/`. It must preserve the layer rule that
the API depends on the engine and the engine never includes API headers.

Google LevelDB supplies `DB`, `Iterator`, `Snapshot`, `WriteBatch`, and three
option structures. The behavior is proven, but owning `DB*` and `Iterator*`
and manually paired `GetSnapshot`/`ReleaseSnapshot` calls make lifetime
correctness the caller's responsibility. Its API also exposes environment,
cache, filter-policy, and logging interfaces before this project has public
implementations of those abstractions.

RocksDB adds column families, transactions, merge operators, rate limiting,
statistics, listeners, secondary instances, and a large option surface.
Those features have no current caller and would make the first facade define
contracts for unimplemented engine behavior.

## Decision

Add these public headers:

```text
include/modern_leveldb/
  db.h
  iterator.h
  options.h
  snapshot.h
  write_batch.h
```

The API uses the existing public `ByteView`, `Error`, `Result`, `Status`, and
`Comparator` types. It is not source- or ABI-compatible with Google LevelDB.

### Options

```cpp
struct Options {
  std::shared_ptr<const Comparator> comparator;
  bool create_if_missing = false;
  bool error_if_exists = false;
  std::size_t write_buffer_size = 4 << 20;
  std::uint64_t max_file_size = 2 << 20;
  std::size_t max_open_files = 1000;
  std::size_t block_size = 4 << 10;
  std::uint32_t block_restart_interval = 16;
  std::optional<std::uint32_t> bloom_bits_per_key;
};

struct ReadOptions {
  const Snapshot* snapshot = nullptr;
  bool fill_cache = true;
};

struct WriteOptions {
  bool sync = false;
};
```

A null comparator selects `BytewiseComparator`. A non-null shared comparator
is retained for as long as any database, iterator, or snapshot handle needs
the engine. A comparator must provide an immutable, deterministic strict total
order; support concurrent calls; keep valid separator and successor bounds;
and use a stable name that uniquely identifies exactly the same semantics on
every reopen. Reusing a name for changed ordering would make persisted table
indexes and level ranges invalid.

Sizes are sanitized by ADR-0037. A zero block restart interval is rejected as
`InvalidArgument`. A Bloom filter is optional; as in LevelDB, a caller that
combines it with a custom comparator must ensure comparator equality implies
byte equality.

Filesystem, executor, clock, block-cache, and concrete filter objects remain
internal. The POSIX backend is the default where ADR-0011 provides it; on a
platform without that backend, `Database::Open` returns `NotSupported`.
Compression is added by the next DAG node rather than represented by a
placeholder option.

### Write batches

```cpp
class WriteBatch final {
 public:
  WriteBatch();
  WriteBatch(const WriteBatch&);
  WriteBatch& operator=(const WriteBatch&);
  WriteBatch(WriteBatch&&) noexcept;
  WriteBatch& operator=(WriteBatch&&) noexcept;
  ~WriteBatch();

  Status Put(ByteView key, ByteView value);
  Status Delete(ByteView key);
  Status Append(const WriteBatch& source);
  void Clear() noexcept;
  std::size_t ApproximateSize() const noexcept;
};
```

The public value owns a private encoded engine batch. It deliberately does
not expose sequence assignment or encoded bytes. Those are persistence and
recovery details. A moved-from batch may only be assigned or destroyed.

### Database and snapshots

```cpp
class Database final {
 public:
  static Result<Database> Open(Options options,
                               std::filesystem::path directory);

  Database(const Database&) = delete;
  Database& operator=(const Database&) = delete;
  Database(Database&&) noexcept;
  Database& operator=(Database&&) noexcept;
  ~Database();

  Status Put(ByteView key, ByteView value,
             const WriteOptions& options = {});
  Status Delete(ByteView key, const WriteOptions& options = {});
  Status Write(const WriteBatch& batch,
               const WriteOptions& options = {});
  Result<std::optional<std::vector<std::byte>>> Get(
      ByteView key, const ReadOptions& options = {});
  Result<Iterator> NewIterator(const ReadOptions& options = {});
  Result<Snapshot> GetSnapshot();
};
```

`Database` is a move-only handle. Its implementation owns a shared engine
state. Each snapshot registers one engine sequence and releases it when its
last internal owner is destroyed:

```cpp
class Snapshot final {
 public:
  Snapshot(const Snapshot&) = delete;
  Snapshot& operator=(const Snapshot&) = delete;
  Snapshot(Snapshot&&) noexcept;
  Snapshot& operator=(Snapshot&&) noexcept;
  ~Snapshot();
};
```

`ReadOptions::snapshot` must name a live snapshot from the same database. A
moved-from snapshot or a snapshot from another database is rejected as
`InvalidArgument`. `Get` retains the registration for the duration of the
read. An iterator retains it for the iterator's whole lifetime, so destroying
the public snapshot handle cannot let a compaction discard entries that the
iterator still reads.

The shared registration control block is allocated before it calls
`DatabaseEngine::GetSnapshot`, and no throwing operation follows that call in
its constructor. An allocation failure therefore occurs before registration.
If the engine insertion itself throws, its multiset operation has the strong
exception guarantee. No sequence can be registered without an owner that
later releases it.

Snapshots and iterators also retain the shared engine state. Destroying the
`Database` handle therefore prevents new operations but closes the engine
only after its last child handle is gone. This replaces LevelDB's manual
"destroy children first" rule with enforced lifetime safety.

A moved-from `Database` rejects operations as `InvalidArgument`.

### Iterators

```cpp
class Iterator final {
 public:
  Iterator(const Iterator&) = delete;
  Iterator& operator=(const Iterator&) = delete;
  Iterator(Iterator&&) noexcept;
  Iterator& operator=(Iterator&&) noexcept;
  ~Iterator();

  bool valid() const noexcept;
  ByteView key() const noexcept;
  ByteView value() const noexcept;
  Status SeekToFirst();
  Status SeekToLast();
  Status Seek(ByteView key);
  Status Next();
  Status Prev();
};
```

An iterator starts invalid. `key` and `value` require a valid position and
remain valid until the iterator moves. Positioning methods return the engine
error directly and leave a failed iterator invalid. A moved-from iterator is
invalid and its positioning methods return `InvalidArgument`.

The database engine remains safe for concurrent calls. A `Database`,
`Snapshot`, `Iterator`, or `WriteBatch` object must not itself be moved,
destroyed, or mutated concurrently with an operation on that same object.

### Layer boundary

The public classes use private implementations in `src/api/`. The current
engine `Database` and encoded `WriteBatch` classes are renamed
`DatabaseEngine` and `EncodedWriteBatch` so that the public names do not leak
engine headers upward or force engine code to include API headers.

No public header includes a file under `src/`.

Private member order enforces every borrowed lifetime:

- The shared database state declares the retained comparator before its
  `DatabaseEngine`, so reverse destruction destroys the engine, including
  background shutdown, before the comparator.
- A snapshot registration releases its sequence in its destructor while it
  still owns the shared database state, then drops that state.
- An iterator implementation declares the shared database state first, its
  optional snapshot registration second, and the private `DbIterator` last.
  Reverse destruction therefore destroys cache handles and callbacks first,
  releases the snapshot second, and drops the engine state last.

Every destructor and move assignment that owns an incomplete private
implementation is defined out of line where that implementation is complete.

## Rejected alternatives

- **Expose the engine headers.** This would make platform injection, table
  options, sequence numbers, test-only waits, and compaction details part of
  the supported API and reverse the required dependency direction.
- **Keep raw owning pointers and manual snapshot release.** That reproduces
  LevelDB's avoidable lifetime hazards instead of using C++23 ownership.
- **Let snapshots or iterators borrow the facade.** Destroying the facade
  first would leave dangling callbacks and table-cache references.
- **Add every LevelDB maintenance method now.** Properties, approximate
  sizes, manual range compaction, repair, and destruction need independent
  engine contracts and current callers before they become public promises.
- **Expose RocksDB-style option families.** They describe unimplemented
  features and violate need-driven simplicity.

## Validation

Unit tests cover:

- Public headers compiling without private include paths.
- Write-batch copy, move, append, clear, binary keys, and validation errors.
- Opening, creating, reopening, Put, Delete, atomic Write, sync writes, Get,
  missing keys, forward and reverse iteration, and seeks.
- Reads and iterators at snapshots.
- An iterator retaining its snapshot registration after the public snapshot
  handle is destroyed.
- Snapshots and iterators keeping the engine locked and usable after the
  `Database` handle is destroyed, followed by successful reopening once the
  last child is gone.
- Rejection of moved-from handles, foreign snapshots, and a zero restart
  interval.
- Bloom-filter option wiring and custom comparator retention.
- `NotSupported` opening through the facade where no default filesystem
  exists.

The existing Debug, Release, sanitizer, coverage, and Google LevelDB
differential gates remain required.

## Consequences

- Applications receive one small, ownership-safe API surface while internal
  engine types remain changeable.
- Child handles may keep the directory locked after the `Database` facade is
  destroyed; this is intentional and replaces undefined lifetime behavior.
- Public options expose only behavior implemented today.
- Compression can extend `Options` in ADR-0039 without changing ownership or
  operation contracts.

## References

- [Google LevelDB `db.h`](https://github.com/google/leveldb/blob/7ee830d02b623e8ffe0b95d59a74db1e58da04c5/include/leveldb/db.h)
- [Google LevelDB `options.h`](https://github.com/google/leveldb/blob/7ee830d02b623e8ffe0b95d59a74db1e58da04c5/include/leveldb/options.h)
- [Google LevelDB `iterator.h`](https://github.com/google/leveldb/blob/7ee830d02b623e8ffe0b95d59a74db1e58da04c5/include/leveldb/iterator.h)
- [Google LevelDB `write_batch.h`](https://github.com/google/leveldb/blob/7ee830d02b623e8ffe0b95d59a74db1e58da04c5/include/leveldb/write_batch.h)
