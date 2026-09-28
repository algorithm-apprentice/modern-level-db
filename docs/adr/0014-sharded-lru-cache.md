# ADR-0014: Typed Sharded LRU Cache

- Status: Accepted
- Date: 2026-09-22

[ADR-0053](0053-leveldb-read-path-parity.md) supersedes the original
`shared_ptr<Entry>` handle and standard-container implementation. The cache
retains its typed API but now uses LevelDB-style intrusive pins, separate
in-use/LRU lists, and a custom hash table on the read hot path.

## Context

SSTable reads repeatedly reuse immutable data blocks. A table cache also needs
to retain opened table/file objects. Both require bounded lookup by binary key,
least-recently-used eviction, and pinning while an iterator or read operation
still references an entry.

This implements only the `implement-cache` DAG node. Block parsing, table
objects, cache-key construction, options, metrics, and table-cache policy remain
separate future nodes.

## Current Modern LevelDB callers

| Future caller | Cache requirement |
|---|---|
| Block cache | Binary key, immutable block value, byte charge, lookup pinning |
| Table cache | Binary file-number key, opened table/file value, unit or resource charge |
| Table reader | A unique cache namespace ID when one cache is shared |
| Memory reporting | Approximate cached charge |
| File cleanup | Erase an obsolete table-cache entry |

No current caller requires priorities, dynamic capacity, strict rejection,
secondary storage, compression, async lookup, admission policy, or cache
plugins.

## Prior art and adopted decisions

### Google LevelDB

Adopt:

- A fixed-capacity LRU cache divided into 16 mutex-protected shards.
- Capacity expressed as caller-supplied charge.
- Lookup and insertion return pinned handles.
- Replacing or erasing a key removes its cache ownership while existing handles
  remain valid.
- Pinned entries are not eviction candidates, so cached charge may exceed
  capacity.
- Zero capacity returns a handle for the inserted value but does not make it
  discoverable by lookup.
- Unique numeric IDs partition cache-key namespaces.

Change:

- Use a typed template rather than `void*`, deleter callbacks, and casts.
- Use move-only RAII handles that release typed intrusive references.
- Ensure value destruction and user-defined shared-pointer deleters never run
  while a shard mutex is held.

### RocksDB

RocksDB retains sharded caches but adds priorities, strict capacity, metadata
charging, secondary caches, custom allocators, dynamic options, and alternative
policies such as HyperClock. These solve workloads and operational requirements
outside the current Modern LevelDB scope and are not adopted.

## Decision

Implement one internal header-only template:

```cpp
template <typename Value>
class ShardedLruCache final {
 public:
  class Handle;

  explicit ShardedLruCache(std::size_t capacity);

  Result<Handle> Insert(
      ByteView key, std::shared_ptr<const Value> value, std::size_t charge);
  std::optional<Handle> Lookup(ByteView key);
  void Erase(ByteView key);

  std::size_t total_charge() const;
  std::uint64_t NewId() noexcept;
};
```

The cache and shards are non-copyable and non-movable. Methods other than
destruction are safe for concurrent calls. The owner must stop cache operations
before destroying the cache.

### Typed RAII handle

`Handle` is move-only. One handle pins one entry through an intrusive
reference protected by its shard mutex.

```cpp
const Value& value() const noexcept;
const Value* operator->() const noexcept;
const Value& operator*() const noexcept;
```

The handle stores the entry and shard that release its reference. It survives
`Erase`, replacement, and eviction. Current block/table callers release all
handles before destroying the owning DB and cache; no cross-cache-lifetime
guarantee is part of the API.

`Insert` rejects a null `shared_ptr` or zero charge with `InvalidArgument`.
Empty binary keys are valid. Values are immutable through cache handles because
blocks and opened-table entries are shared among concurrent readers.

### LRU and pinning

Each shard owns a custom separate-chaining hash table, one circular in-use
list, one circular unpinned LRU list, cached charge, fixed shard capacity, and
one mutex.

The cache owns one intrusive reference. Every handle owns another. Lookup of an
unpinned entry moves it from LRU to in-use; releasing its last handle moves it
back to the most-recent end of LRU. Insertion returns a pinned in-use entry.

Eviction removes only the least-recent unpinned entries. Pinned entries remain
in the hash table and in-use list, so insertion may leave cached charge above
capacity when nothing is evictable. Releasing a handle does not synchronously
evict; the next insertion enforces capacity again, matching LevelDB.

`Erase` removes only the current mapping. Existing handles to a replaced or
erased entry remain valid.

### Sharding and capacity

- The cache uses exactly 16 shards.
- `Hash32(key, 0)` selects a shard from the high four hash bits.
- Each shard receives `ceil(total_capacity / 16)`, matching LevelDB. Therefore
  effective aggregate capacity may exceed the requested capacity by at most
  15 charge units. A requested zero capacity gives every shard zero capacity.
- Eviction is local to a shard. The cache does not borrow unused capacity from
  another shard.
- `total_charge()` sums each shard under its mutex and saturates at
  `std::numeric_limits<std::size_t>::max()` instead of wrapping. It is suitable
  for diagnostics but is not one globally atomic snapshot while concurrent
  mutations occur.

### Lock and destruction rules

The shard mutex protects the hash table, both lists, charge, and intrusive
reference transitions.

- Key/value/entry allocation happens before acquiring the shard mutex.
- Hash-table resize and retirement storage may allocate under the mutex but
  execute no user-provided callbacks.
- Entries whose final reference is removed are retired and destroyed after
  unlocking.
- Handle destruction releases through its shard but destroys the value only
  after unlocking.
- Cache destruction requires external exclusion and no remaining handles under
  the current DB ownership contract.

### Charge overflow

Before mutating a shard, insertion computes the post-replacement charge.
If adding the requested charge would overflow `std::size_t`, it returns
`InvalidArgument` and leaves the current mapping unchanged.

### Unique IDs

`NewId()` uses an atomic monotonically increasing `std::uint64_t`, starting at
one. Wraparound is outside the practical lifetime of a process and is not
given a reuse protocol.

## Explicitly deferred behavior

- Runtime capacity changes.
- Strict capacity rejection for pinned entries.
- Multiple priority pools or scan-resistant admission.
- Manual prune operations.
- Metadata charge estimation.
- Hit/miss/eviction metrics.
- Secondary, compressed, persistent, or remote caches.
- Custom allocators and cache-policy interfaces.
- A non-template public cache ABI.

## Validation plan

Unit tests cover:

- Hit, miss, insertion, replacement, and erase.
- Move-only handles and value lifetime after erase/replacement.
- LRU recency and charge-based eviction.
- Pinned entries exceeding capacity and later eviction.
- Zero-capacity behavior.
- Empty/binary keys and transparent lookup.
- LevelDB-style shard capacity and saturating charge accounting.
- Charge-overflow failure atomicity.
- Concurrent lookup, insertion, erase, and ID allocation.
- User-provided value deleters executing outside shard locks.
- Hash-table growth and collisions within one shard.
- Move assignment releasing the previous intrusive pin.

ThreadSanitizer exercises concurrent operations. Performance tuning waits until
the table/block callers and benchmarks exist.

## Consequences

- Block and table caches share one type-safe implementation without a general
  `void*` cache API or per-lookup shared-pointer reference counting.
- Move-only RAII handles preserve automatic release while matching LevelDB's
  intrusive cache ownership and eviction decisions.
- Fixed per-shard capacity can evict from a busy shard while another shard has
  unused space, matching the simplicity of the initial LevelDB model.

## References

- [Google LevelDB cache interface](https://github.com/google/leveldb/blob/7ee830d02b623e8ffe0b95d59a74db1e58da04c5/include/leveldb/cache.h)
- [Google LevelDB sharded LRU implementation](https://github.com/google/leveldb/blob/7ee830d02b623e8ffe0b95d59a74db1e58da04c5/util/cache.cc)
- [RocksDB cache options](https://github.com/facebook/rocksdb/blob/main/include/rocksdb/cache.h)
