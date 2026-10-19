# API and Options

[User reference](README.md)

The public facade is move-only and RAII-owned. Fallible operations return
`Result<T>` or `Status`; normal storage errors are values rather than
exceptions.

## Database operations

| Operation | Contract |
|---|---|
| `Database::Open` | Opens or creates one owned database directory |
| `Put` / `Delete` | Applies one update, optionally with a WAL sync |
| `Write` | Copies and atomically applies a `WriteBatch` |
| `WriteExclusive` | Exclusively borrows a batch during the call; no concurrent access to that batch |
| `Get` | Returns presence plus reusable or owning value output |
| `NewIterator` | Creates an initially invalid bidirectional iterator |
| `GetSnapshot` | Returns an RAII visibility pin |
| `GetState` | Returns an owning, no-I/O snapshot of LSM and maintenance state |

`GetState()` returns a `DatabaseStateSnapshot`, an independent point-in-time
value that remains valid after later database activity or destruction.
Its fields can be modified without affecting the database. Unlike the
`Snapshot` returned by `GetSnapshot()`, it does not pin read visibility and
cannot be supplied through `ReadOptions`.

An iterator is initially invalid. Call `SeekToFirst`, `SeekToLast`, or `Seek`
before reading it; `key()` and `value()` require `valid()==true`. Their
`ByteView` values remain valid only until the iterator moves or is destroyed.
Do not operate on one iterator concurrently from several threads.

Keep a snapshot alive for every read that names it, and do not move or destroy
it concurrently with those reads. `Database`, `Iterator`, and `WriteBatch`
operations reject their moved-from state; a moved-from snapshot is rejected
when supplied through `ReadOptions`.

## `Options`

| Field | Default | Meaning |
|---|---:|---|
| `comparator` | null | Uses the bytewise comparator; a custom shared comparator is retained |
| `create_if_missing` | `false` | Create an absent database |
| `error_if_exists` | `false` | Reject opening an existing database |
| `write_buffer_size` | 4 MiB | Soft memtable arena-memory threshold |
| `max_file_size` | 2 MiB | Target compaction output size |
| `max_open_files` | 1000 | Database open-file budget before non-table reservations |
| `allow_mmap_reads` | `true` | Permit exact-size immutable table mappings |
| `block_size` | 4 KiB | Approximate uncompressed data-block size |
| `block_restart_interval` | 16 | Keys between full-key restart entries |
| `bloom_bits_per_key` | unset | Enable the built-in Bloom policy |
| `compression` | Snappy | None, Snappy, or Zstd |
| `zstd_compression_level` | 1 | Zstd level in the supported -5 through 22 range |
| `sync_wal_creation` | `true` | Sync the initial empty WAL and request the backend namespace barrier before it accepts writes; later rotation requests the namespace barrier |
| `allow_weak_namespace_durability` | `false` | Required for the owned native Windows backend |
| `file_system` | null | Retain and use a supplied filesystem instead of creating a native backend |
| `block_cache` | null | Retain a supplied decoded-block cache instead of creating an 8 MiB cache |
| `clock` | null | Retain a supplied monotonic clock instead of using the system clock |
| `executor` | null | Retain a supplied executor instead of creating a serial worker |

The engine clips `max_open_files` to 74 through 50,000,
`write_buffer_size` to 64 KiB through 1 GiB, `max_file_size` to 1 MiB through
1 GiB, and `block_size` to 1 KiB through 4 MiB. Unsupported compression
values, invalid Zstd levels, and invalid table/filter settings return explicit
errors. See [`options.h`](../../include/modern_leveldb/options.h) for public
types and defaults.

`write_buffer_size` is measured using memtable arena reservations, including
per-block pointer accounting. It is not exact payload size or a hard allocation
cap; one write group can exceed the threshold.

## Supplying runtime dependencies

The four dependency fields are independent `shared_ptr` values. Opening copies
or moves those owners into the database's retained state. Iterators and
snapshots retain that state too, so supplied services can safely outlive the
original `Database` handle without requiring the caller to keep another owner.
The engine closes before those retained owners are released. Sharing protects
lifetime, not the thread safety of a custom implementation.

The public contracts are
[`file_system.h`](../../include/modern_leveldb/file_system.h),
[`clock.h`](../../include/modern_leveldb/clock.h), and
[`background_executor.h`](../../include/modern_leveldb/background_executor.h).
Implementations can derive from these interfaces. `CreateDefaultFileSystem`
returns an admitted native backend that a wrapper can delegate to; its returned
exclusive owner can also be moved into `Options::file_system`.

An injected filesystem controls its own mapping, admission, and durability
policy. `allow_mmap_reads` and `allow_weak_namespace_durability` configure the
default backend, not a replacement backend. All returned file handles must
be destroyed before their backend.

An executor must accept work for deferred, exactly-once eventual execution,
including during database shutdown. It must never run work inline in
`Schedule`; a failed or throwing schedule must not retain or execute it.
Do not destroy a database while blocking the only event loop that can run its
accepted work. The engine waits for its tasks but does not stop a supplied
executor that other owners may still use.

Create an opaque block cache through
[`block_cache.h`](../../include/modern_leveldb/block_cache.h):

```cpp
modern_leveldb::Options options;
options.block_cache = modern_leveldb::CreateBlockCache(16 * 1024 * 1024);
auto second_database_options = options;
```

Both option sets share the same cache budget; table-specific namespace IDs
keep entries from different databases distinct. Capacity is an eviction
target, not an exact memory cap, and pinned entries can postpone eviction.
Zero capacity disables cache retention. `BlockCacheCharge` observes retained
block charges, not total process or allocator memory. Private decoded blocks
and custom eviction-policy plugins are not part of this public interface.

## Read and write options

`ReadOptions` selects an optional live snapshot and whether newly read blocks
may enter the block cache. Existing cache hits, the table cache, and the OS
cache remain available when `fill_cache=false`.

`WriteOptions::sync=true` flushes the committed WAL contents before the write
is acknowledged. It does not turn the whole database into a transaction and
does not strengthen a backend's namespace guarantee.

## Comparator and Bloom requirements

A custom comparator must:

- keep the same semantic identity on every reopen;
- provide deterministic ordering and equality;
- support concurrent calls;
- preserve separator/successor bounds.

Changing its name does not migrate existing sorted data. Enable
`bloom_bits_per_key` only when comparator equality implies byte equality;
otherwise a Bloom filter can report equivalent keys missing.

## Deliberate API differences from LevelDB

Modern uses RAII snapshots and iterators instead of manual release/delete.
It exposes typed `GetState()` rather than arbitrary string properties.
The public facade does not currently expose manual range compaction,
approximate range sizes, repair/destroy helpers, a combined Env, injected
logger/filter objects, custom cache-policy implementations, log reuse, or a
checksum-disable switch.
