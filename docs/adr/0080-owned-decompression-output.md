# ADR-0080: Final Owned Decompression Output

- Status: Proposed
- Date: 2026-10-21

## Scope and implementation status

This is a design-only proposal. Decompression ownership is not changed by
this ADR, and implementation requires approval after design review.
The separately requested single-call read CRC change does not depend on it.

The goal is to decode directly into final, address-stable storage without
pre-initializing bytes that the codec will overwrite. It is not a claim that
output initialization explains the complete random-Get performance gap.
Fully cached reads also retain a measured gap.

Public APIs, persisted bytes, codec choices, cache policy, exception behavior,
and data-validation rules remain unchanged. Compressor scratch buffers,
LookupKey, comparator interfaces, and iterator algorithms are outside scope.

## Existing ownership boundary

The current path is:

```text
DecompressBlock -> vector<byte> -> BlockContents -> Block -> cache or iterator
```

`BlockContents` also accepts an already-owned uncompressed vector without
copying its allocation, or borrows uncompressed mapped file bytes.
Its owner outlives block iterators. Moving a Block preserves the backing
addresses of existing iterators.

`vector` cannot adopt the allocation owned by `unique_ptr<byte[]>`.
Decoding to an array and then constructing a vector would add an allocation
and a byte copy. The new array must therefore reach the existing ownership
boundary directly, not be converted back into a vector.

## Proposed design

### A two-field decoder result

Add a small, private table-layer struct in `src/table/compression.h`:

```cpp
struct DecodedBlockBytes {
    std::unique_ptr<std::byte[]> data;
    std::size_t size;

    DecodedBlockBytes(std::unique_ptr<std::byte[]> bytes,
                      std::size_t length) noexcept;
    DecodedBlockBytes(DecodedBlockBytes&& other) noexcept;
    DecodedBlockBytes& operator=(DecodedBlockBytes&&) = delete;
};
```

The required constructor precondition is that the owner covers `size` fully
initialized bytes. A null owner is permitted only for size zero.
Copying is unavailable. The move constructor transfers the pointer and
exchanges the source size with zero, keeping a moved-from result safely empty.
Assignment is unnecessary for this immutable handoff, as with BlockContents.
There are no growth, iterator, allocator, or generic-container interfaces.

Change only the private decoder result:

```cpp
Result<DecodedBlockBytes> DecompressBlock(ByteView stored,
                                        BlockCompression type);
```

Each codec validates its declared output size before allocation, allocates
with `std::make_unique_for_overwrite<std::byte[]>(size)`, and writes directly
to that allocation. Only successful, completely validated output is wrapped
in DecodedBlockBytes and returned.

Empty Snappy output retains the current zero-output handling and need not
allocate an array. Zstd's existing rejection of zero or unknown declared
length remains unchanged.

### Three mutually exclusive BlockContents sources

Retain the existing BlockContents boundary and use one private storage
discriminated union:

```cpp
using Storage =
    std::variant<ByteView, std::vector<std::byte>, DecodedBlockBytes>;
```

| Source | Stored alternative | Allocation handoff | Independently cacheable |
|---|---|---|---|
| Decoded compressed block | DecodedBlockBytes | Move the array owner; no conversion to vector | Yes |
| Already-owned uncompressed bytes | vector<byte> | Move the existing vector allocation | Yes |
| Borrowed uncompressed mapped bytes | ByteView | Retain the existing view | No |

Keep `Owned(vector<byte>)` and `Borrowed(ByteView)`, and add a typed
`Owned(DecodedBlockBytes)` factory. All expose the existing `data()` ByteView.
Determine cacheability from the alternative, never from whether a pointer
is null. Owned empty bytes and borrowed empty bytes remain distinct.

Do not store all three representations simultaneously, introduce shared
ownership, or add a type-erased allocator/deleter framework.

### Integration and dependency direction

The private codec result depends only on standard-library storage and base
bytes/results. Compression does not start depending on BlockContents or Block,
so the current dependency direction remains intact.

| Consumer | Required change |
|---|---|
| `compression.h/.cc` | Return the two-field owner after successful decoding |
| `block_format.h/.cc` | Represent the three sources and adopt decoded owners directly |
| `Block::Create(vector<byte>)` and filter-block construction | Preserve existing vector adoption |
| `Block::Create(BlockContents)` and Table::ReadDataBlock | Preserve the current owning handoff |
| Codec tests and format fuzzer | Inspect the returned pointer/length as a ByteView |
| Public Database, Iterator, Snapshot, and filesystem APIs | No change |

The resulting compressed path is:

```text
codec -> unique_ptr<byte[]> + size -> BlockContents -> Block -> existing consumer
```

No payload copy occurs during those ownership transfers.
The vector and mapped-view paths do not pass through the decoded-array form.

## Invariants and failures

- Every nonempty published decoded range is fully initialized by a successful
  codec operation. Partially written output is never published.
- Preserve the current uint32 block-size and container-limit admission bounds,
  including the existing behavior on narrower platforms; do not silently
  replace them with a larger allocation limit.
- Preserve checksum-before-decompression ordering, codec-length validation,
  exact-size validation, existing error codes/messages, and malformed-input
  rejection. Allocation failures continue to propagate as exceptions.
- Allocation and codec failure release the local array owner through RAII.
  There is no fallback that turns failed decoding into successful empty data.
- Moving DecodedBlockBytes, BlockContents, or Block transfers ownership
  without relocating any backing bytes. Existing block iterators and values
  retain their addresses for their documented lifetimes.
- Mapped input does not make decompressed output borrowed. Only uncompressed,
  stable mapped bytes use the borrowed alternative.
- The mapped file still outlives its borrowed blocks. Cache charge remains
  the decoded data length, not allocation capacity or metadata size.
- Do not separately repack Layout or change cache retirement under this ADR.

## Alternatives not selected

Small stack output followed by a heap fallback does not satisfy the current
escaping-block contract. Copying stack output into final cache-owned storage
adds a copy. A specialized no-cache, synchronous consumption path could use
temporary stack bytes, but would require different control flow and is not
part of this proposal. The configured block size is a flush target, not an
upper bound on an individual record or decoded block.

Embedding small byte arrays inside a movable Block would relocate backing
bytes. A string held by value has the same risk through small-string storage.
A fixed-address string owner with `resize_and_overwrite` could preserve
addresses, but normally adds a separate object allocation for larger output.

`reserve` followed by append is valid when the producer supplies elements;
the current native codec APIs instead require writable contiguous output.
Writing into unused vector capacity, or manipulating vector internals to
adopt a raw allocation, is not an acceptable replacement.

## Verification and delivery boundary

Before implementation, independently review this ownership design and its
existing-caller integration. Keep this ADR Proposed until it is approved.

Implementation then uses small test-first increments:

- Verify decoded pointer/length contents and move-to-empty behavior, including
  empty, small binary, and large output.
- Extend BlockContents tests to prove zero-copy adoption and address stability
  for vector, array, and borrowed alternatives, including owned/borrowed empty
  contents and destruction of compressed source storage.
- Extend the existing Block-move/iterator-validity test to decoded storage.
- Preserve real Snappy/Zstd round trips, length and codec-error cases, cache
  charge, and table behavior. Successful codec test doubles must initialize
  every byte they claim to produce; tests must not inspect unwritten output.
- Update the format fuzzer's result inspection without weakening its
  allocation budget or byte-equality checks.

Use the existing focused tests and cross-platform sanitizer/coverage gates.
After correctness and ownership review, compare complete workloads under the
existing fixed measurement controls. Do not substitute object-size ratios
or isolated initialization timings for end-to-end evidence, and do not
promise a fixed speedup.

Stop after this private storage handoff. Generic buffers, small-buffer
thresholds, public result redesign, and unrelated read-path changes require
separate decisions.
