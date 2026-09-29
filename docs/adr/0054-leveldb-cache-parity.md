# ADR-0054: Pinned LevelDB Cache Parity

## Status

Accepted design. The 2026-09-29 amendment below resolves implementation-review
findings about variable-size deallocation, unapproved entry metadata, and
double-probe insertion. Merge and review the amendment before restarting
ADR-0053 Milestone 1.

## Context

ADR-0053 requires Modern LevelDB to complete the pinned LevelDB read-path
baseline before further optimization experiments. Its first milestone replaces
the read caches' `shared_ptr<Entry>`, `unordered_map`, and `list` hot path with
the ownership and eviction model in Google LevelDB revision
`7ee830d02b623e8ffe0b95d59a74db1e58da04c5`.

The first implementation draft began after only the route-level ADR. It used
the correct broad ideas—intrusive references, in-use/LRU lists, and a custom
hash table—but made layout, pimpl, retirement, value ownership, overflow, and
coverage decisions during coding. PR #63 was closed before merge. No cache
implementation change reached `main`.

This ADR fixes the complete cache design before code.

## Reference structure

The pinned `util/cache.cc` has exactly four implementation layers:

1. `LRUHandle`: one variable-size allocation containing ownership metadata and
   inline key bytes.
2. `HandleTable`: a separate-chaining hash table from key/hash to
   `LRUHandle*`.
3. `LRUCache`: one mutex-protected shard with one hash table, one unpinned LRU
   list, and one pinned in-use list.
4. `ShardedLRUCache`: sixteen `LRUCache` shards plus the unique-ID counter.

Modern keeps those four roles. It does not introduce a cache-policy interface,
pimpl layer, second cache lookup, or future SIEVE abstraction.

Name the non-template implementation `ShardedLruCacheCore`. Keep
`ShardedLruCache<Value>` as the typed façade used by callers. Do not use the
ambiguous `IntrusiveLruCache` name for a second-looking cache object: the core
contains the only shards, hash tables, lists, and policy state.

## `LRUHandle` mapping

| Pinned field | Modern field | Decision |
|---|---|---|
| `void* value` | `const void* value` in the private core entry | The typed façade is the only creator/accessor |
| deleter function pointer | typed static deleter installed by `ShardedLruCache<Value>` | Same type-erased destruction mechanism behind a typed API |
| `next_hash` | `Entry* next_hash` | Identical hash-chain role |
| `next` / `prev` | `Entry* next` / `previous` | Identical circular-list role |
| `charge` | `std::size_t charge` | Identical accounting unit |
| `key_length` | `std::size_t key_length` | Identical key boundary |
| `in_cache` | `bool in_cache` | Identical cache-reference state |
| `refs` | `std::uint32_t refs` | Identical cache plus handle reference count |
| `hash` | `std::uint32_t hash` | Same `Hash32(key, 0)` value |
| trailing `key_data` | trailing bytes after the fixed core `Entry` | Same single-allocation key locality |

The core performs one allocation of:

```text
sizeof(Entry) + key.size()
```

It placement-constructs the fixed entry and copies the key immediately after
it. The entry stores no key pointer or offset. The value object already exists
before insertion, as it does in LevelDB; the cache adds no separate key
allocation or shared-reference control block.

`ShardedLruCache<Value>::Insert` accepts `unique_ptr<const Value>`, validates
it, releases its raw pointer to the core, and supplies a static deleter that
casts back to `const Value*`. `Handle::value()` performs the corresponding
typed cast. The private core owns the pointer exclusively in success, error,
uncached, replacement, erase, eviction, and destruction paths.

The entry exposes its key as a borrowed `ByteView` beginning at
`reinterpret_cast<const byte*>(entry) + sizeof(Entry)`. Sentinel list heads
use a non-value entry and never expose a key.

Entries are not deleted with a C++ `delete` expression. The core invokes the
typed value deleter, explicitly destroys the fixed `Entry`, and releases the
variable-size storage with unsized `::operator delete(void*)`. This exactly
pairs with the original allocation and cannot select sized deallocation using
only `sizeof(Entry)`.

## `HandleTable` mapping

Implement the pinned algorithm directly:

- Start with four null buckets.
- Select a bucket with `hash & (length - 1)`.
- Use the repository's LevelDB-compatible `Hash32(key, 0)` and retain golden
  hash-vector tests against the pinned implementation.
- `FindPointer` walks `next_hash` while either the full hash or key differs.
- `Lookup` returns `*FindPointer`.
- `PrepareInsert` calls `FindPointer` exactly once and records the slot,
  existing entry, and whether a new key would cross the bucket-count
  threshold.
- `CommitInsert` replaces an equal entry in place. For a new key, it links the
  entry, increments the element count, and produces the same final resize and
  hash-chain order as resizing only after `elements > bucket_count`.
- `Remove` unlinks one hash-chain entry and decrements the element count.
- `Resize` chooses the smallest power of two at least as large as the element
  count and rehashes existing entries without allocating entries.

Use a raw bucket array owned by `std::unique_ptr<Entry*[]>`. Do not use
`std::unordered_map` or a bucket `std::vector`; their policy, node allocation,
and iterator behavior are not the chosen baseline.

If a prepared new-key insertion needs growth, `CommitInsert` allocates the new
bucket array before mutation. After allocation succeeds, it links the entry
through the prepared old-table slot and rehashes exactly as the pinned
post-insert `Resize`. No operation after mutation begins can throw. Allocation
failure leaves the old table, mappings, count, and charge intact. Replacement
and non-growing insertion allocate nothing and use the prepared slot directly.

## One-shard `LRUCache` mapping

Each shard owns, inline:

- Capacity and current cached charge.
- One mutex.
- One `HandleTable`.
- One circular `lru` sentinel.
- One circular `in_use` sentinel.

The invariants match the pinned source:

- Cached entries appear in exactly one list.
- `refs == 1 && in_cache` means unpinned and on `lru`.
- `refs >= 2 && in_cache` means pinned and on `in_use`.
- An erased/replaced entry with live handles has `in_cache == false` and is on
  neither list.
- The LRU head's `next` is oldest and `previous` is newest.

### `Ref`

If an entry has only the cache reference, move it from `lru` to `in_use`, then
increment `refs`.

### `Unref`

Decrement `refs`:

- At zero, require `!in_cache` and retire the entry for destruction.
- At one while cached, move it from `in_use` to the newest end of `lru`.

### `Lookup`

Under the shard mutex, find the entry and call `Ref`. Return one move-only typed
RAII handle. The lookup does not copy a `shared_ptr`, allocate, or perform a
second cache lookup.

### `Insert`

Before locking:

1. Allocate the fixed core variable-size entry with the typed value pointer
   and deleter supplied by the façade.
2. Copy its inline key.
3. Set `refs = 1` for the returned handle.

Under the shard mutex:

1. Call `HandleTable::PrepareInsert` once.
2. Compute post-replacement charge from the prepared existing entry.
3. If capacity is nonzero and charge accounting can represent the insertion,
   call `CommitInsert`. It may allocate growth buckets before mutation, but
   never probes the key chain again.
4. After commit succeeds, add the cache reference, mark `in_cache`, append to
   `in_use`, finish erasing any replaced entry, and update charge.
5. Evict oldest `lru` entries while usage exceeds capacity.
6. If capacity is zero or charge accounting would overflow, leave the new
   entry uncached with its returned-handle reference only.

Return a handle in every valid insertion case. Null values and zero charge
remain `InvalidArgument`. Charge overflow is a Modern safety exception to
LevelDB's unchecked addition: it returns a usable uncached handle and leaves
all mappings/charge unchanged.

### `FinishErase`

For a non-null entry already removed from the hash table:

- Remove it from its current list.
- Clear `in_cache`.
- Subtract charge.
- Drop the cache reference through `Unref`.

### Destruction outside the mutex

Pinned LevelDB calls the deleter while holding the shard mutex. Modern retains
ADR-0014's reentrancy and deadlock safety: final value destruction happens
after unlocking.

Do not allocate retirement storage. Once an entry is removed from the hash
table, temporarily reuse its `next_hash` field to link a local retired chain.
After unlocking, destroy that chain in order. This adds no miss-path allocation
relative to LevelDB.

### Destructor

External exclusion and zero live handles are preconditions. Assert `in_use` is
empty. For every LRU entry, save its next list link, clear `in_cache`, and drop
the cache reference through `Unref`; the zero-reference path therefore retains
its required `!in_cache` invariant. Link each resulting entry into the retired
chain and destroy that chain after the traversal. The cache does not promise
that handles survive cache destruction.

## `ShardedLRUCache` mapping

The non-template core owns an inline `std::array<Shard, 16>`, not a pimpl.

- Select a shard from the high four hash bits.
- Per-shard capacity is `ceil(total_capacity / 16)`.
- `Insert`, `Lookup`, `Release`, and `Erase` dispatch once to that shard.
- `total_charge` sums shard usage.
- `NewId` returns monotonically increasing nonzero IDs.

Modern retains an atomic ID counter instead of LevelDB's separate ID mutex.
This is strictly no more read-path work and does not change ID semantics.

`total_charge` saturates instead of wrapping on cross-shard addition. This is a
diagnostic safety exception outside lookup/insert hot paths.

## Approved deviations from pinned LevelDB

No other deviation may be introduced during implementation without amending
and reviewing this ADR.

| Pinned behavior | Approved Modern behavior | Reason |
|---|---|---|
| Public `void*` value/deleter API | Private core `const void*`/typed static deleter behind a `unique_ptr` façade | Same exclusive layout and destruction with type-safe callers |
| Public virtual `Cache` policy interface | Concrete typed façade over one concrete core | Avoid a virtual dispatch and reject premature policy abstraction |
| Entry allocation while holding shard mutex | Entry/value/key allocation before locking | Same allocation work, shorter critical section, strong exception safety |
| Insert new hash link, then resize | One `PrepareInsert` probe; preallocate growth buckets, then commit the same link/rehash result | Allocation failure must leave mappings and counts unchanged |
| Explicit caller `Release(handle)` | Move-only typed RAII handle | Automatic release with the same one-reference semantics |
| Null values and zero charge accepted by raw API | Typed `Insert` rejects them before entry allocation | Preserve the existing Modern internal cache contract |
| Deleter runs from `Unref` under mutex | Intrusive retired chain destroyed after unlocking | Prevent user destruction/reentrancy under the cache lock without allocation |
| Unchecked `usage += charge` | Overflow yields a successful uncached handle | Preserve the value and mappings without integer wrap |
| ID mutex | Relaxed atomic monotonically increasing ID | Same semantics with no greater hot work |
| Cross-shard total may wrap | Saturating diagnostic total | Defined behavior outside the lookup path |
| `Prune()` maintenance API | Omitted until a caller exists | Not used by the pinned point-read path or current Modern API |
| `malloc` with implicit process-level OOM handling | Throwing C++ allocation before mutation | Repository-standard allocation/error semantics |

## Typed façade

Keep the existing internal name and call-site shape:

```cpp
template <typename Value>
class ShardedLruCache {
 public:
  class Handle;

  Result<Handle> Insert(
      ByteView key, std::unique_ptr<const Value> value, std::size_t charge);
  std::optional<Handle> Lookup(ByteView key);
  void Erase(ByteView key);
  std::size_t total_charge() const;
  std::uint64_t NewId() noexcept;
};
```

The template contains only:

- Null/zero validation and transfer from `unique_ptr<const Value>`.
- The typed static value deleter.
- `Handle::value()` typed access.
- Conversion between typed handles and the non-template core pin.

Hashing, locking, refs, lists, table operations, capacity, eviction, erase,
retirement, IDs, and charge all live in one non-template core. The façade does
not exist to support alternative policies.

## Call-site ownership changes

### Table cache

`TableCache::Find` moves its opened `unique_ptr<Table>` into the cache. A
successful insertion returns the only read pin. Unit charge cannot overflow in
practical table-cache state; the generic uncached-overflow behavior remains
valid.

### Block cache

`Table::ReadDataBlock` constructs one `unique_ptr<const Block>`.

- Without a block cache or with `fill_cache = false`, `BlockReference` owns the
  unique pointer.
- With insertion enabled, ownership moves into the cache entry and
  `BlockReference` owns the returned handle.
- Zero capacity or charge overflow returns an uncached handle; no
  `shared_ptr<Block>` fallback exists.

`BlockReference` therefore holds either one cache handle or one
`unique_ptr<const Block>`.

### No remaining cache shared ownership

After this milestone, neither the cache implementation nor its block/table
callers may contain:

- `shared_ptr<Entry>`
- `shared_ptr<const Block>`
- `shared_ptr<const Table>`

Unrelated memtable/version/public-handle `shared_ptr`s remain for ADR-0053
Milestone 4.

## Exception safety

- Entry/value/key allocation finishes before the shard lock.
- One `PrepareInsert` probes the hash chain once.
- Hash-table growth allocates its new bucket array before mutating chains,
  then commits the pinned post-insert chain/resize result without another
  probe.
- After mutation begins, list/table/ref/charge operations are `noexcept`.
- Retirement uses existing entry links and cannot allocate.
- Invalid insert arguments fail before locking.
- A failed entry or bucket allocation leaves cache contents unchanged.
- User value destruction happens only after unlocking.
- Variable-size entry storage is released through explicit unsized
  deallocation, never a sized deleting destructor.

## Validation plan

### Structural mapping

Tests or static assertions cover:

- Exactly 16 shards and LevelDB's ceiling capacity.
- `refs` transitions for cache-only, first pin, additional pins, last release,
  erase, replacement, and eviction.
- Entries appearing in exactly one or zero lists as required.
- Oldest/newest LRU ordering.
- Same-hash/different-key collision chains.
- Hash-table growth after crossing bucket count and replacement without
  growth.
- Zero capacity.
- Pinned charge exceeding capacity.
- Uncached handle on charge overflow with mappings unchanged.
- Monotonic concurrent IDs and saturating total charge.

### Ownership

- Lookup does not allocate.
- Lookup and additional handles do not create shared ownership.
- Entry and inline key use one allocation, excluding the separately created
  value object.
- Variable-size entry destruction reaches the typed deleter and matching
  unsized storage deallocation under ASan/UBSan and a sized-deallocation build.
- Erased/replaced/evicted values survive until the last handle releases.
- Reentrant value destruction can call back into the cache, proving it occurs
  outside the mutex.
- Block and table cache call sites transfer `unique_ptr` ownership.

### Repository gates

Run:

- Full native unit and public API tests.
- Targeted ASan/UBSan and TSan cache/table-cache tests.
- LevelDB compatibility/model/crash tests.
- AppleClang Debug/Release and GCC warning-clean builds.
- Changed-code line and branch coverage at 100%.
- The standalone allocation test uses `_aligned_malloc`/`_aligned_free` on
  Windows and `posix_memalign`/`free` on POSIX so every required CI platform
  builds the same allocation contracts.

Do not run a cache-only throughput admission test. Performance is measured only
after every ADR-0053 milestone is complete.

## Rejected alternatives

### Keep `shared_ptr<const Value>` inside an intrusive entry

Rejected. It removes per-hit entry reference increments but retains a separate
control block, atomic ownership, and miss/eviction overhead without any
independent owner that needs them.

### Pimpl the sharded core

Rejected. It adds one allocation and pointer indirection relative to the
pinned inline shard array, and it obscures the direct structural mapping.

### Allocate key bytes separately

Rejected. LevelDB's one-allocation entry/key layout is part of the chosen cache
baseline.

### Retirement vector

Rejected. Lock-safe destruction does not require an allocation; removed
entries can reuse their hash link as an intrusive retired chain.

### General cache policy interface

Rejected. SIEVE, priorities, strict capacity, and alternate policies are
post-parity work and must not shape this implementation.

## Consequences

- Milestone 1 has a direct field/function-level reference design before code.
- The cache removes all shared ownership from table/block cache values and
  handles.
- Modern retains type safety, RAII release, lock-safe destruction, and explicit
  overflow behavior without adding lookup/miss allocations relative to the
  reference.
- Any implementation deviation requires an ADR amendment and review before
  code changes.

## Delivery boundary

Obtain one bounded GPT-5.6 Sol review of this ADR against the pinned
`util/cache.cc`. Resolve every material finding and merge the design-only PR.
Only then create a fresh Milestone-1 implementation branch. The abandoned PR
#63 and local draft are not implementation inputs except as examples of
decisions this ADR now fixes explicitly.

## References

- [ADR-0014 typed sharded LRU cache](0014-sharded-lru-cache.md)
- [ADR-0053 pinned LevelDB read-path parity](0053-leveldb-read-path-parity.md)
- [Pinned LevelDB cache implementation](https://github.com/google/leveldb/blob/7ee830d02b623e8ffe0b95d59a74db1e58da04c5/util/cache.cc)
