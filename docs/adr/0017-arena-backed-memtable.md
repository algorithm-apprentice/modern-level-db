# ADR-0017: Arena-Backed MemTable

- Status: Accepted
- Date: 2026-09-23

## Context

The mutable MemTable must publish ordered internal-key/value entries after WAL
durability, serve snapshot-aware point reads without locks, and provide ordered
iteration for future flush and merged-read paths.

This implements only the `implement-memtable` DAG node. Write-batch
application, WAL ordering, mutable-to-immutable rotation, ownership across DB
versions, flush scheduling, and merged DB iteration remain later engine nodes.

## Current Modern LevelDB callers

| Future caller | Required behavior |
|---|---|
| Write path | Add one validated Put/Delete entry after WAL success |
| Recovery | Reinsert decoded write-batch entries |
| Point-read path | Distinguish miss, value, and deletion at a snapshot |
| Flush path | Iterate all internal keys and values in sorted order |
| Merged iterator | Seek and traverse internal-key entries |
| MemTable rotation | Read arena-reserved memory usage on the writer thread |

No current caller requires multiple writers, in-place updates, merge operands,
range tombstones, prefix Bloom filters, hash tables, insertion hints, batched
multi-get, or pluggable MemTable representations.

## Prior art and adopted decisions

### Google LevelDB

Adopt:

- One externally serialized writer and lock-free concurrent readers.
- One arena owning packed immutable entries and skip-list nodes.
- The exact packed entry representation:

```text
entry :=
  internal-key-size varint32
  user-key bytes
  trailer fixed64 little-endian
  value-size varint32
  value bytes
```

- Skip-list ordering by the length-prefixed internal key.
- Snapshot lookup through a seek key containing
  `(user key, snapshot sequence, Value kind)`.
- A 200-byte inline lookup-key buffer with heap fallback for long keys.
- Point reads that distinguish a missing user key from a deletion tombstone.
- Iterators exposing borrowed internal-key and value views.

Change:

- Use RAII object ownership instead of intrusive `Ref`/`Unref`.
- Return an explicit lookup-kind enum instead of overloading a boolean and
  mutable status output.
- Return typed validation errors from insertion and lookup-key construction.
- Return an iterator object directly rather than heap-allocating a polymorphic
  public iterator.

### RocksDB

RocksDB preserves length-prefixed MemTable keys and arena skip-list storage but
adds pluggable representations, concurrent insert paths, insertion hints,
multi-get, merge operands, range tombstones, prefix filters, checksums, and
many accounting modes. Modern LevelDB rejects those mechanisms because no
current caller requires them.

### Pebble

Pebble's arena skip list stores immutable versioned internal keys and values,
uses tombstones rather than physical deletion, and reports duplicate internal
keys as insertion errors. It also adds concurrent writers, backward links,
splice caching, and offset-based arenas. Modern LevelDB retains only the
immutable-entry and duplicate-rejection principles; its existing single-writer
skip list remains sufficient.

## Decision

### Lookup key

Extend the internal-key module with:

```cpp
class LookupKey final {
 public:
  static Result<LookupKey> Create(
      ByteView user_key, SequenceNumber sequence);

  LookupKey(const LookupKey&) = delete;
  LookupKey& operator=(const LookupKey&) = delete;
  LookupKey(LookupKey&& source) noexcept;
  LookupKey& operator=(LookupKey&& source) noexcept;

  ByteView memtable_key() const noexcept;
  ByteView internal_key() const noexcept;
  ByteView user_key() const noexcept;
};
```

Its bytes are:

```text
varint32(user-key-size + 8)
user-key
fixed64((sequence << 8) | Value)
```

`memtable_key()` includes the length prefix. `internal_key()` begins at the
user key, and `user_key()` excludes the trailer.

Construction rejects sequences above `MaxSequenceNumber`, user keys whose
internal-key size cannot fit uint32, and complete representations that cannot
fit `size_t`. Keys whose complete lookup representation fits 200 bytes use
inline storage; longer keys use one heap allocation. LookupKey is move-only.
Move construction and assignment reset the source to the canonical lookup key
for an empty user key at sequence zero, preserving a valid packed-key invariant
without allocation.

The lookup kind is `Value`, the maximum supported original-LevelDB kind, so the
seek lands on the newest entry whose sequence does not exceed the snapshot.

### Direct varint encoding

The packed arena representation requires writing a varint into preallocated
storage. Add this coding primitive:

```cpp
bool EncodeVarint32(MutableByteView& output, std::uint32_t value) noexcept;
```

It writes the canonical varint at the beginning of `output` and advances the
view past the encoded bytes. If the output is too short, it returns `false`
without changing the view or its bytes. Existing vector append and borrowed
consume APIs remain unchanged.

### MemTable API

```cpp
enum class MemTableLookupKind {
  Missing,
  Value,
  Deletion,
};

struct MemTableLookup {
  MemTableLookupKind kind;
  ByteView value;
};

class MemTable final {
 public:
  explicit MemTable(const Comparator& user_comparator);
  MemTable(Comparator&&) = delete;
  MemTable(const Comparator&&) = delete;

  Status Add(
      SequenceNumber sequence,
      ValueKind kind,
      ByteView key,
      ByteView value);

  MemTableLookup Lookup(const LookupKey& key) const;
  std::size_t memory_usage() const noexcept;

  class Iterator {
   public:
    explicit Iterator(const MemTable& table) noexcept;

    bool valid() const noexcept;
    ByteView key() const;
    ByteView value() const;
    void Next();
    void Prev();
    Status Seek(ByteView internal_key);
    void SeekToFirst();
    void SeekToLast();
  };
};
```

MemTable and Iterator are non-copyable and non-movable. The MemTable owns its
internal-key comparator, arena, packed entries, and skip list. It borrows the
configured user comparator, which must outlive the MemTable and be safe for
concurrent const calls. Temporary comparator construction is rejected.

`MemTableLookup::value` and iterator views borrow immutable arena storage and
remain valid until MemTable destruction. Missing and deletion results have an
empty value view; an empty stored value is distinguished by lookup kind.

### Entry ordering and parsing

The skip-list key is a pointer to the packed entry. Its comparator decodes only
the leading internal-key length and delegates to `InternalKeyComparator`.
Every stored and temporary seek key is constructed internally and therefore
valid; unchecked prefix parsing is confined to the MemTable implementation.

Iterator `key()` omits the leading length prefix and returns the complete
encoded internal key. `value()` omits its length prefix. `Seek` accepts an
encoded internal key, builds a temporary length-prefixed search key in
iterator-owned scratch storage, and reports `InvalidArgument` if the key length
cannot fit uint32 or if prefix plus key cannot fit `size_t`. A validation
failure leaves the iterator position unchanged.

### Insertion

`Add` validates before arena allocation:

- Sequence is at most `MaxSequenceNumber`.
- Kind is `Value` or `Deletion`.
- `key.size() + 8` and `value.size()` fit uint32.
- The complete encoded entry size fits `size_t`.

It then allocates the exact encoded size, fully writes immutable bytes, and
publishes the entry through `SkipList::Insert`. Empty keys and values are
valid. A deletion may carry bytes for original-LevelDB compatibility, although
normal write-batch callers provide an empty value.

Equal complete internal keys are rejected with `InvalidArgument`. Because the
arena does not free individual allocations, a duplicate or a later skip-list
allocation failure may consume reserved arena bytes while leaving logical
contents unchanged. Normal DB sequence assignment makes duplicates invalid
caller behavior.

### Snapshot lookup

`Lookup` seeks with `LookupKey::memtable_key()`. If the first candidate's user
key does not compare equal through the configured user comparator, it returns
`Missing`. Otherwise:

- `Value` returns a borrowed value view.
- `Deletion` returns the deletion kind and an empty value.

No separate sequence check is needed: internal-key ordering and the lookup
trailer skip entries newer than the snapshot.

### Concurrency and lifetime

- `Add` requires external single-writer synchronization.
- `Lookup` and independent iterators may run concurrently with `Add`.
- Entry bytes are fully initialized before the skip list publishes their
  pointer with a release store.
- Readers reach entries through acquire loads and therefore observe initialized
  key/value bytes.
- Entries and nodes are never individually removed or reclaimed.
- MemTable destruction requires all readers and iterators to have stopped.
- `memory_usage()` is called by the serialized writer or after external
  synchronization; the Arena counter itself is not atomic.

## Explicitly deferred behavior

- Applying an entire `WriteBatchReader`.
- WAL-before-MemTable orchestration and writer grouping.
- Mutable/immutable MemTable ownership and rotation.
- Flush scheduling and SSTable construction.
- A shared engine iterator interface and merged iteration.
- Snapshots as owned engine objects.
- Intrusive reference counting or hazard-style reclamation.
- Concurrent writers, insertion hints, or cached splices.
- Merge, range deletion, single deletion, or additional value kinds.
- Prefix Bloom filters, hash representations, and multi-get.
- Exact logical-byte accounting separate from arena reserved bytes.

## Validation plan

Unit tests cover:

- Exact lookup-key bytes, inline/heap boundaries, binary keys, and moves.
- Empty MemTables and memory accounting.
- Value, deletion, miss, empty-value, and snapshot-boundary lookups.
- Multiple versions of one user key and custom user-comparator equality.
- Exact internal-key iterator order, seek, forward traversal, and reverse
  traversal.
- Invalid sequence/kind rejection and duplicate internal-key behavior.
- Randomized point reads against an ordered reference model.
- Single-writer/concurrent-reader publication of immutable keys and values.

The concurrency test uses latches without sleeps, observes the first inserted
entry only through MemTable lookup, and validates every key/value reached
through concurrent iteration. It runs under ThreadSanitizer. A session-only
differential helper compares insertion, point lookup, and iteration with
unmodified LevelDB.

## Consequences

- MemTable reads allocate nothing for ordinary keys and acquire no mutex.
- Flush receives original-LevelDB-compatible internal keys in sorted order.
- Arena lifetime makes returned views cheap but requires explicit higher-layer
  lifetime pinning.
- The packed representation minimizes per-entry overhead at the cost of
  internal unchecked parsing guarded by construction invariants.
- Extended operation kinds or alternate MemTable structures require a later
  explicit design decision.

## References

- [Google LevelDB MemTable](https://github.com/google/leveldb/blob/7ee830d02b623e8ffe0b95d59a74db1e58da04c5/db/memtable.cc)
- [Google LevelDB LookupKey](https://github.com/google/leveldb/blob/7ee830d02b623e8ffe0b95d59a74db1e58da04c5/db/dbformat.h)
- [RocksDB MemTable](https://github.com/facebook/rocksdb/blob/main/db/memtable.h)
- [RocksDB skip-list representation](https://github.com/facebook/rocksdb/blob/main/memtable/skiplistrep.cc)
- [Pebble arena skip list](https://github.com/cockroachdb/pebble/blob/master/internal/arenaskl/skl.go)
