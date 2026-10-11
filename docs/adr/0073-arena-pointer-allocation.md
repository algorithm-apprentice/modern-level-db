# ADR-0073: Arena Pointer Allocation Interface

- Status: Accepted

## Context

Arena originally returned `MutableByteView` from `Allocate(bytes)` and
`AllocateAligned(bytes)`. The view paired the allocated pointer with the same
length the caller had just supplied.

The production callers already retain that length:

- MemTable knows the complete encoded-entry size and needs a bounded byte view
  while writing the entry.
- SkipList knows the node-plus-links allocation size and only needs an aligned
  address for placement construction.

Returning the length again does not communicate allocator-derived information.
It also makes the raw-storage allocator appear responsible for a caller's
logical byte range. Google LevelDB's Arena returns a pointer and treats the
positive allocation size as a caller precondition.

## Decision

Arena exposes:

```cpp
std::byte* Allocate(std::size_t bytes);
std::byte* AllocateAligned(std::size_t bytes);
```

Both functions require `bytes > 0`; Debug builds assert that precondition.
Returned storage remains stable until Arena destruction.

A caller that needs a bounded byte range constructs it from the pointer and
the size it already owns:

```cpp
MutableByteView output(arena.Allocate(encoded_size), encoded_size);
```

Callers performing placement construction use the pointer directly.

## Consequences

- Arena has one responsibility: allocate stable raw storage.
- The return value no longer repeats the input length.
- Aligned object construction no longer unwraps a span only to recover its
  pointer.
- Byte-encoding callers remain explicit about the size that defines their
  writable view.
- Zero-byte allocation is not supported because no implemented consumer needs
  it and a raw pointer would give it ambiguous semantics.
- The ABI returns one pointer-sized value instead of a pointer/size pair,
  although this is not treated as a material performance optimization.

## Validation

- Arena tests wrap returned pointers in views when testing writable extents and
  retained contents.
- Debug death tests cover the positive-size precondition.
- Alignment, block growth, dedicated large blocks, current-block reuse, and
  memory accounting retain their existing coverage.
- MemTable, SkipList, concurrency, sanitizer, compatibility, and performance
  tests validate the two production consumers.

## References

- [Original Arena decision](0008-monotonic-arena.md)
- [Memory and MVCC lesson](../learning/03-memory-and-mvcc.md)
- [Pinned LevelDB Arena](https://github.com/google/leveldb/blob/7ee830d02b623e8ffe0b95d59a74db1e58da04c5/util/arena.h)
