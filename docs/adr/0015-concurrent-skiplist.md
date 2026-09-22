# ADR-0015: Single-Writer Concurrent-Reader Skip List

- Status: Accepted
- Date: 2026-09-22

## Context

The future MemTable requires an ordered in-memory index with inexpensive
insertion, lower-bound seek, forward iteration, and concurrent reads while one
externally synchronized writer inserts new entries. MemTable entries are never
removed individually; the complete arena and skip list share one lifetime.

This implements only the `implement-skiplist` DAG node. Memtable entry
encoding, internal-key comparison adapters, snapshots, and write
synchronization remain separate future nodes.

## Current Modern LevelDB caller

The only planned production caller is MemTable:

- One DB writer queue serializes insertions.
- Multiple read threads perform `Contains`, `Seek`, and forward iteration.
- Keys are immutable views or pointers into the MemTable arena.
- Nodes and keys remain allocated until the complete MemTable is destroyed.
- MemTable destruction occurs only after readers can no longer access it.

No current caller requires concurrent writers, deletion, update, reclamation,
range views, map values, or lock-free ownership reclamation.

## Prior art and adopted decisions

### Google LevelDB

Adopt:

- Maximum height 12 and branching factor 4.
- One externally synchronized writer.
- Lock-free readers using acquire loads.
- Release publication of fully initialized nodes.
- Relaxed maximum-height observation because stale heights are safe.
- No deletion and arena lifetime for all nodes.
- `Seek`, `Next`, `Prev`, first, and last navigation.

Change:

- Use project naming and `Arena::AllocateAligned`.
- Return `false` for an attempted duplicate insertion instead of relying only
  on a debug assertion.
- Avoid LevelDB's trailing `atomic<Node*> next_[1]` allocation technique.
  Allocate and construct the variable-height atomic link sequence separately
  in arena storage, then store its pointer in a trivially destructible node.
  This keeps C++ object lifetimes explicit and standard-compatible.
- Reject temporary comparator objects because the skip list borrows its
  comparator.

### ConcurrentSkipListMap and general concurrent skip lists

General-purpose concurrent maps support multiple writers, deletion, helping,
logical tombstones, and memory reclamation. Modern LevelDB does not adopt those
mechanisms because MemTable already serializes writes and never deletes nodes.
Adding them would increase atomic states and proof surface without a caller.

## Decision

Implement an internal template:

```cpp
template <typename Key, typename Compare>
class SkipList final {
 public:
  explicit SkipList(const Compare& compare, Arena& arena);

  bool Insert(Key key);
  bool Contains(const Key& key) const;

  class Iterator {
   public:
    explicit Iterator(const SkipList& list) noexcept;

    bool valid() const noexcept;
    const Key& key() const;
    void Next();
    void Prev();
    void Seek(const Key& target);
    void SeekToFirst();
    void SeekToLast();
  };
};
```

`Key` must be default-initializable, trivially copy-constructible, and trivially
destructible because keys live in arena nodes whose individual destructors are
never run. Its alignment must not exceed `alignof(std::max_align_t)`, matching
the current Arena contract; over-aligned key types are rejected by the template
constraint. `Compare` returns a negative, zero, or positive integer, implements
a strict weak ordering, and is safe for concurrent const calls. Its ordering
and zero-equivalence must remain stable for the list's complete lifetime.
Comparator state and any data referenced by stored keys must not mutate in a
way that changes ordering.

The skip list and iterator are non-copyable and non-movable. The borrowed
comparator and arena must outlive the list and all iterators. Construction from
a comparator rvalue is deleted.

### Node layout and lifetime

Each node contains:

- One immutable `Key`.
- Its height.
- A pointer to separately arena-allocated raw byte storage containing
  individually constructed `std::atomic<Node*>` objects.

The link storage is not treated as a C++ array object. For each level, the
implementation computes a byte offset from the storage base, casts that exact
address, and placement-constructs one atomic object. Link access repeats the
same byte-offset calculation and uses `std::launder`; it never performs typed
pointer indexing across separately constructed objects. A static assertion
requires the atomic object's size to preserve its alignment at every offset.

Both the links and node are created in aligned arena storage. Every link atomic
is initialized before the node is published. The supported key and atomic
types are required to be trivially destructible, so arena destruction may
reclaim their raw storage without individual destructor calls.

The head node has maximum height and a default-constructed dummy key that is
never returned to callers.

### Writer protocol

`Insert` requires external writer synchronization.

1. Search top-down and record the predecessor at every level.
2. If an equal key already exists, return `false` without allocating.
3. Choose a height using LevelDB's deterministic Park-Miller generator seeded
   with `0xdeadbeef`; each additional level has probability `1/4`.
4. Fully construct the node and initialize its outgoing links with relaxed
   operations.
5. If needed, publish a larger maximum height with a relaxed store.
6. Link the node from level zero upward with release stores.
7. Return `true`.

There is no rollback path after publication begins. Arena allocation failure
occurs before publication and follows standard C++ allocation behavior.

### Reader protocol

Readers load links with acquire semantics. Observing a published node therefore
also observes its immutable key and initialized outgoing links.

The maximum height uses relaxed load/store:

- A stale lower value only makes the reader start at a lower level.
- A reader that observes a newer height before the corresponding head link sees
  `nullptr` and descends.

If insertion completion happens-before a read operation through higher-layer
synchronization, that read must observe the inserted key. An insertion
concurrent with a read may or may not be observed, but readers must never see a
partially initialized node, invalid key, cycle, or backward ordering.

### Iterator semantics

- `Iterator(const SkipList&)` binds the iterator to one list and starts
  invalid.
- `Seek` positions at the first key greater than or equal to the target.
- `SeekToFirst` and `SeekToLast` position at the extrema.
- `Next` follows level zero in constant time.
- `Prev` performs a top-down search and is logarithmic on average; no backward
  links are stored.
- `key`, `Next`, and `Prev` require a valid iterator.
- An iterator may observe nodes inserted concurrently after its creation.
- The list and arena must outlive every iterator.

### Random height

The fixed deterministic generator is internal implementation state, not a
security or user-visible random source. Tests do not depend on an exact
individual height sequence; they verify structure and behavior.

## Explicitly deferred behavior

- Multiple writers or internal writer locking.
- Deletion, replacement, or memory reclamation.
- Values or map operations.
- Backward links or optimized reverse scans.
- Runtime height/branching configuration.
- Custom allocator concepts beyond `Arena`.
- Approximate size or statistics.

## Validation plan

Unit tests cover:

- Empty-list behavior.
- Ordered insertion, duplicate rejection, contains, seeks, first/last,
  forward iteration, and reverse iteration.
- Randomized comparison against `std::set`.
- Binary/view keys backed by arena-stable storage.
- Arena memory growth and node persistence.
- Comparator and arena lifetime constraints.
- A deterministic single-writer/multiple-reader stress model with
  self-validating immutable keys inserted into interleaved gaps. Readers check
  checksum and global order without a per-node external publication atomic.

ThreadSanitizer runs the concurrent stress test. Unit tests remain bounded and
avoid arbitrary sleeps by using latches and atomic stop flags.

## Consequences

- MemTable receives the concurrency model it actually needs without general
  lock-free map complexity.
- Readers perform no lock acquisition.
- Nodes and links are never individually reclaimed, making pointer safety
  depend on the MemTable/Arena lifetime invariant.
- Reverse iteration is slower than forward iteration, matching the original
  LevelDB tradeoff.

## References

- [Google LevelDB skip list](https://github.com/google/leveldb/blob/7ee830d02b623e8ffe0b95d59a74db1e58da04c5/db/skiplist.h)
- [Google LevelDB skip-list tests](https://github.com/google/leveldb/blob/7ee830d02b623e8ffe0b95d59a74db1e58da04c5/db/skiplist_test.cc)
- [C++ atomic memory ordering](https://eel.is/c++draft/atomics.order)
- [Java `ConcurrentSkipListMap`](https://docs.oracle.com/en/java/javase/21/docs/api/java.base/java/util/concurrent/ConcurrentSkipListMap.html)
