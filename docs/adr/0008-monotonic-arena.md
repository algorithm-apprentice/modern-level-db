# ADR-0008: Monotonic Arena

- Status: Accepted
- Date: 2026-09-21

## Context

Memtables and their skip-list nodes allocate many small objects that share one
lifetime. Individually freeing those objects adds bookkeeping, pointer traffic,
and allocator overhead without providing useful reclamation: the complete
memtable is discarded only after it has been flushed and is no longer visible
to readers.

This implements only the `implement-arena` DAG node. Skip lists, memtables, and
concurrent publication remain separate future nodes.

## Decision

Implement an internal `Arena` in the `memory` layer with these operations:

| API | Contract |
|---|---|
| `Allocate(std::size_t bytes)` | Return a mutable byte view of exactly `bytes` bytes. No alignment beyond byte alignment is promised. |
| `AllocateAligned(std::size_t bytes)` | Return a mutable byte view aligned to `alignof(std::max_align_t)`. |
| `memory_usage()` | Return the exact total byte capacity of allocation blocks currently owned by the arena. |

The returned views are non-owning. Non-empty allocations remain valid and
retain their contents until the arena is destroyed. Individual allocations
cannot be released.

### Ownership and movement

The arena exclusively owns all backing blocks through RAII. It is neither
copyable nor movable, so its identity and the lifetime anchor for returned
views remain stable. Destruction releases every backing block.

### Allocation policy

- Zero-byte allocation returns an empty view and reserves no memory.
- Small allocations come from 4 KiB backing blocks.
- An allocation first uses the current block when it fits. If fallback is
  required, a request larger than one quarter of a backing block is placed in
  its own block and does not replace the current small-allocation block.
- When the current block cannot satisfy a small allocation, its remaining
  bytes are intentionally abandoned and a new 4 KiB block becomes current.
- Aligned allocation may consume padding in the current block. Padding is part
  of reserved memory but not exposed in the returned view.

`memory_usage()` counts the capacities of owned byte blocks. It intentionally
excludes the `Arena` object, the block-owner vector, allocator metadata, and
unused virtual or allocator capacity outside the requested blocks.

### Failure and concurrency

Allocation may throw `std::bad_alloc`, following the standard C++ allocation
contract. Resource exhaustion is not normal storage-engine control flow and is
not converted into `Status`. If block allocation or ownership registration
throws, no newly allocated block is retained and previously returned views
remain valid.

The arena is not thread-safe. Allocation and `memory_usage()` require external
synchronization. Higher layers may publish fully initialized objects for
concurrent reading, but that publication protocol belongs to the skip-list and
memtable nodes.

The initial arena does not implement:

- Individual deallocation or reset.
- User-selected block sizes.
- User-selected or over-aligned allocation.
- Concurrent allocation.
- A standard-library allocator or `std::pmr::memory_resource` adapter.

These are omitted until an implemented consumer demonstrates a requirement.

## Consequences

- Allocation is constant-time in the common small-object path.
- Bulk destruction is proportional to the number of backing blocks, not the
  number of allocated objects.
- Some bytes at the end of small blocks and before aligned allocations are
  intentionally wasted.
- Objects constructed in arena memory must be trivially destructible or have
  destruction managed by their owning higher layer; the arena destroys raw
  storage, not individual objects.

## Validation

Unit tests cover zero-size behavior, exact returned sizes, alignment, data
survival across block growth, non-overlap, dedicated large blocks, reuse of the
current small block after a large allocation, exact reserved-byte accounting,
and copy/move restrictions. Sanitizer builds validate bounds and lifetime
behavior.

The implementation is informed by Google LevelDB's arena allocation policy,
but exposes byte views and RAII ownership rather than owning raw pointers.

## Reference

- [Google LevelDB arena](https://github.com/google/leveldb/blob/7ee830d02b623e8ffe0b95d59a74db1e58da04c5/util/arena.cc)
