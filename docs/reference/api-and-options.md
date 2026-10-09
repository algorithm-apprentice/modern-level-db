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
| `write_buffer_size` | 4 MiB | Memtable switch threshold |
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

The engine clips `max_open_files` to 74 through 50,000,
`write_buffer_size` to 64 KiB through 1 GiB, `max_file_size` to 1 MiB through
1 GiB, and `block_size` to 1 KiB through 4 MiB. Unsupported compression
values, invalid Zstd levels, and invalid table/filter settings return explicit
errors. See [`options.h`](../../include/modern_leveldb/options.h) for public
types and defaults.

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
approximate range sizes, repair/destroy helpers, a custom Env, an injected
block cache/logger/filter object, log reuse, or a checksum-disable switch.
