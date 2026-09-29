# ADR-0056: Pinned LevelDB Table and Mapped-Block Parity

## Status

Accepted design. Bounded GPT-5.6 Sol review added the independent mapped-page
storage-fault warning, completed the `BlockContents` construction contract,
and corrected the uninjectable POSIX fallback validation claims. Merge this
design-only ADR before ADR-0053 Milestone 3 implementation.

## Context

ADR-0053 requires the complete pinned Google LevelDB read path before the
cohesive path receives one final performance decision. Milestone 1 replaced
the table and block caches with pinned intrusive ownership. Milestone 2, merged
in PR #68, replaced eager block scans with lazy checked decoding, added the
trusted internal-key boundary, and propagated real block corruption through
tables, merged iterators, database iterators, and compaction.

The next remaining boundary is the relationship among table files, decoded
blocks, the block cache, and the POSIX random-access implementation.

Modern already has the accepted ADR-0050 mmap mechanism:

- `RandomAccessFile::TryReadView` may return exact immutable file bytes.
- The POSIX backend may map an exact-size table and otherwise falls back to
  `pread`.
- Compressed input is checksummed and decompressed directly from a stable
  mapped view.
- The public `allow_mmap_reads` option currently defaults to false.
- The process-wide production budget currently permits 1,000 mappings and
  at most 4 GiB of mapped bytes.

ADR-0050 deliberately copied mapped uncompressed blocks into owning vectors
and allowed them into the block cache. That isolated the file-access
experiment from block ownership. It is now the exact ownership difference
that ADR-0053 Milestone 3 must remove.

Pinned LevelDB's `ReadBlock` distinguishes three outcomes:

1. A copied uncompressed block owns the scratch buffer and is cacheable.
2. A mapped uncompressed block borrows the file mapping and is not cacheable.
3. A compressed block always owns its decompressed bytes and is cacheable,
   regardless of whether its compressed input came from a mapping or scratch.

Pinned `Table::BlockReader` inserts only cacheable blocks. A borrowed block is
used for the current lookup or iterator and then its small `Block` object is
destroyed; the mapped bytes remain owned by the open table file.

Modern currently erases that distinction:

- Both `DecodeStoredBlock` overloads return an owning `vector<byte>`.
- `Block` can own only a vector.
- Every successfully read data block may enter the block cache.
- A mapped uncompressed table therefore copies bytes and double-caches them.

The current `Table::Get` control flow already follows pinned
`Table::InternalGet` after Milestone 2:

1. Seek the index.
2. Consult the filter.
3. Look up or read the data block.
4. Seek the data block.
5. Parse the found internal key and compare its user key.
6. Return data-block corruption and retain the validated index invariant.

Modern uses stack iterators and typed results instead of pinned heap iterators
and a callback. Those mechanisms perform no additional hot-path work and are
strictly cheaper by construction. Milestone 3 must not replace them merely
for allocation-level identity. Reusable public result ownership remains
Milestone 4.

## Scope

This ADR covers only ADR-0053 Milestone 3:

- Decoded block bytes that are either owned or borrowed.
- Mapped-uncompressed block cacheability and lifetime.
- Copied-uncompressed and compressed block ownership.
- Index, metaindex, filter, and data-block use of the same ownership result.
- The production POSIX mmap default and count-only limiter.
- An explicit copied-read opt-out.
- File-access provenance and read-diagnostic schema changes caused by the new
  default.
- Directly related ADRs, public documentation, tests, and fuzz contracts.

It does not cover:

- Lazy version/file visitation or seek charging.
- Memtable/version read pins.
- Public reusable output.
- Table properties, prefetch, readahead, `madvise`, direct I/O, async I/O, or
  partial-file mappings.
- Windows mappings or a new non-POSIX backend.
- Recovery from external modification or truncation of a live mapped table.
- A Milestone-3-only throughput admission test.

The project is alpha and has no production deployments. Public and internal
interfaces may break to reach the smallest coherent final design. Do not add
aliases, migrations, dual-mode validators, or compatibility shims for the
current option defaults or diagnostic artifacts.

## Pinned field and function mapping

| Pinned LevelDB mechanism | Modern mechanism | Decision |
|---|---|---|
| `BlockContents::data` | `BlockContents::data()` | Return decoded bytes without exposing the storage representation |
| `BlockContents::cachable` | `BlockContents::cacheable()` | Derive from owned versus borrowed decoded storage |
| `BlockContents::heap_allocated` | owning `vector<byte>` alternative | RAII replaces manual `delete[]` |
| `ReadBlock` scratch/mapped distinction | owning-vector and borrowed-view decode overloads | Preserve the input ownership distinction for uncompressed blocks |
| `Block` over a borrowed `Slice` | `Block` over `BlockContents` | Block metadata may borrow bytes whose file owner outlives it |
| `Table::Rep::file` | `Table::file_` | Declare first so it is destroyed last |
| `Table::Rep::index_block` | `Table::index_` | May borrow the table mapping |
| `Table::Rep::filter_data` and filter | `FilterBlockReader` holding `BlockContents` | The reader owns decompressed/copied bytes or borrows mapped bytes |
| `Table::BlockReader` cache test | `Table::ReadDataBlock` | Insert only cacheable blocks |
| uncached `Block` cleanup | `BlockReference` owning `unique_ptr<const Block>` | RAII deletes metadata while the table keeps borrowed bytes alive |
| cached block cleanup | `BlockCache::Handle` | Existing intrusive pin/release already matches the pinned lifetime |
| POSIX `Limiter mmap_limiter_` | count-only `PosixMmapBudget` | Match the 1,000-region decision and remove the byte budget |
| default `NewRandomAccessFile` mmap | default `PosixFileSystem` mmap | Map exact-size immutable tables by default on supported 64-bit POSIX |
| test-set mmap limit | injected shared test budget | Retain deterministic isolated tests without a process-global mutation hook |

## Decoded block ownership

### Add one move-only `BlockContents` type

Add a move-only decoded-storage type in `table/block_format.h`:

```cpp
class BlockContents final {
 public:
  [[nodiscard]] static BlockContents Owned(
      std::vector<std::byte> contents) noexcept;
  [[nodiscard]] static BlockContents Borrowed(ByteView contents) noexcept;

  BlockContents(const BlockContents&) = delete;
  BlockContents& operator=(const BlockContents&) = delete;
  BlockContents(BlockContents&&) noexcept = default;
  BlockContents& operator=(BlockContents&&) = delete;

  [[nodiscard]] ByteView data() const noexcept;
  [[nodiscard]] bool cacheable() const noexcept;

 private:
  explicit BlockContents(std::vector<std::byte> contents) noexcept;
  explicit BlockContents(ByteView contents) noexcept;

  std::vector<std::byte> owned_;
  std::optional<ByteView> borrowed_;
};
```

`Borrowed` is an internal lifetime contract: its caller guarantees that the
source remains immutable and alive for the `BlockContents` lifetime. The
optional distinguishes an owned empty block from a borrowed view; it is not a
nullable success fallback.

`cacheable()` is true exactly when the decoded bytes are owned. It does not
mean that a caller requested cache insertion.

Do not store a `ByteView` into `owned_`. `data()` computes the view from the
active representation so moving `BlockContents` cannot leave a view pointing
at a moved-from vector object.

### Preserve ownership in both decode overloads

Change both `DecodeStoredBlock` overloads to return
`Result<BlockContents>`.

For an owning stored vector:

- Validate trailer size, checksum, and compression type first.
- Uncompressed: remove the trailer in place and return `Owned`.
- Snappy/Zstd: decompress into a new vector and return `Owned`.

For a borrowed stored view:

- Perform the same validation first.
- Uncompressed: return `Borrowed` over the contents before the trailer, with
  no copy.
- Snappy/Zstd: decompress into a vector and return `Owned`.

The compressed-input view is consumed synchronously. A decompressed result
never retains a pointer into the mapped input.

Existing diagnostics continue to count stored bytes, decoded bytes, and
decompression exactly once in the decoder. Ownership does not add a new timed
stage or candidate-specific counter.

## Block, filter, and table lifetime

### Let `Block` hold `BlockContents`

`Block` owns one `BlockContents` instead of one vector. Keep a direct
`Block::Create(vector<byte>)` convenience for unit tests, fuzzing, and block
builders; it wraps the vector as owned decoded bytes.

The table reader uses `Block::Create(BlockContents)`. `Block::size()` reports
decoded bytes and `Block::cacheable()` delegates to the storage object.

The existing `Layout` may retain a `ByteView` into the active storage:

- Moving an owned vector transfers its allocation, preserving the pointer.
- Moving borrowed storage preserves the external pointer.
- The existing guarantee that iterators remain valid when their `Block`
  moves therefore remains valid.

Construction and iterator validation rules from ADR-0055 do not change.

### Let filters use the same storage contract

`FilterBlockReader` stores `BlockContents`, not an unconditional vector.
Its offset validation and `KeyMayMatch` read through `contents_.data()`.

This matches pinned LevelDB:

- A mapped uncompressed filter borrows the table file.
- A copied or decompressed filter owns its bytes.

The filter reader remains move-only and does not expose borrowed bytes.

### Lock the table destruction order

`Table::file_` remains the first declared data member, so C++ destroys it
last. The index block and filter reader may therefore borrow the mapping
through their entire lifetimes.

For a data block:

- `Table::Get` holds its `BlockReference` until key parsing and value copying
  finish.
- `Table::Iterator` requires its table to outlive it and declares the block
  reference before the block iterator, so the iterator is destroyed first.
- Higher-level iterators hold a `TableCache::Handle` before constructing a
  `Table::Iterator`.

No new mapping pin, shared ownership, deferred-unmap queue, or block-to-table
back pointer is needed.

## Table lookup and block-cache policy

`ReadStoredBlock` keeps the current safety boundary:

1. Validate the handle range against the MANIFEST file size.
2. Prefer an exact stable file view.
3. Otherwise allocate and complete short reads through `ReadExactly`.
4. Decode checksum and compression into `BlockContents`.

`ReadBlock` creates a `Block` from that storage without changing ownership.
The footer remains a one-time copied `ReadExactly` operation so complete
short-read/truncation behavior stays explicit, as ADR-0053 already approved.

`Table::ReadDataBlock` retains the pinned order:

1. Build the table-ID/block-offset cache key.
2. Use an existing cache entry regardless of `fill_cache`.
3. On a miss, read and validate the block.
4. Reject a block with no reachable position.
5. Insert only when the block is cacheable and `fill_cache` is true.
6. Otherwise return an uncached owning `BlockReference`.

The four data-block cases are therefore:

| Stored source | Compression | Decoded storage | Block cache |
|---|---|---|---|
| copied | none | owned, reusing the stored allocation | eligible |
| mapped | none | borrowed from table file | never inserted |
| copied | Snappy/Zstd | owned decompressed vector | eligible |
| mapped | Snappy/Zstd | owned decompressed vector | eligible |

A noncacheable block still performs the normal cache lookup first. Repeated
reads of a mapped uncompressed block therefore miss, checksum the mapped
bytes, and create temporary block metadata as pinned LevelDB does.

Cache charge remains decoded block size. `fill_cache = false` still uses
existing entries but inserts neither owned nor borrowed misses.

### Keep the current `InternalGet` representation

Do not introduce pinned LevelDB's heap iterators, result callback, `void*`
argument, or manual cleanup functions.

The current stack index/data iterators and `BlockReference` implement the same
decision points with fewer allocations. ADR-0055's complete physical index
validation makes index movement infallible after open; data movement errors
remain explicit typed results. The current typed lookup result stays until
Milestone 4 adds reusable caller output.

## POSIX mmap default

### Make the public default mapped

Use one boolean and make mapped reads the default:

```cpp
struct Options {
  // Uses POSIX mmap for exact-size immutable table files where supported.
  // Mapped page storage faults may terminate with SIGBUS instead of returning
  // typed Io. Set false to force copied reads and typed read errors.
  bool allow_mmap_reads = true;
};
```

`DatabaseEngineOptions` uses the same default. The default owned
`PosixFileSystem` receives this value. A custom injected filesystem continues
to control its own random-access implementation; the public flag does not wrap
or override it.

On unsupported address widths the production mmap limit is zero, so the
effective default remains copied reads. Non-POSIX builds retain their existing
backend requirements.

`PosixFileSystem()` itself selects the production mmap budget. The explicit
boolean constructor remains the internal/test mechanism:

- `PosixFileSystem(true)` uses the production budget.
- `PosixFileSystem(false)` forces `pread`.

Applications that require typed I/O failure isolation set
`allow_mmap_reads = false`.

### Replace the count/byte budget with a count-only limiter

The production budget becomes the pinned count:

```text
64-bit POSIX: 1,000 concurrent read-only full-file mappings
32-bit POSIX: 0 mappings
```

Remove:

- The 4-GiB production byte limit.
- Mapped-byte reservation and release accounting.
- `MmapAcquireResult::BytesExhausted`.
- The production/test byte-budget constructor argument.
- The byte-budget-exhausted runtime fallback reason from the new diagnostic
  schema.

Use LevelDB-style atomic acquire/release decisions. Modern may keep the
limiter in a `shared_ptr` so injected test limiters safely outlive a
`PosixFileSystem`; this reference increment occurs only while opening/closing
a file and is outside the Get hot path.

Tests inject a maximum mapping count. Do not expose a public production limit.

### Retain Modern's open-boundary safety exceptions

Continue to map only when:

- An expected MANIFEST size is present and nonzero.
- The expected size fits `size_t`.
- `fstat` succeeds.
- The actual and expected sizes match exactly.
- A mapping slot is available.
- `mmap` succeeds.

Count exhaustion, absent/mismatched size, unsupported size/address width, and
resource-dependent `mmap` failure fall back to the existing opened `pread`
file. These are approved Modern safety/availability deviations from pinned
LevelDB, whose mmap setup errors may fail the open.

The descriptor closes after a successful full-file mapping. The mapping owns
the file reference until `munmap`, and releasing the mapped file returns one
limiter slot.

## Supported file-ownership contract

While a database is open:

- Its directory is owned by that database process.
- Another database process cannot open the directory because of the lock.
- External actors must not modify, replace, or truncate live database files.
- SSTables are immutable and Modern never truncates one.

Obsolete-file cleanup retains:

```text
prove the file is not live or pending
-> evict the table-cache entry
-> release every table handle
-> destroy/unmap the table
-> unlink the path
```

Unlinking an already mapped file remains safe on supported POSIX systems.
External live truncation may produce `SIGBUS`; under the supported ownership
contract this is a contract violation, not a recoverable database input.

Separately, a storage error encountered while faulting a valid mapped page may
also terminate the process with `SIGBUS` even when no actor violated the
immutable-file contract. This is an mmap failure-mode tradeoff, not an
ownership-contract violation. Applications that require random-read storage
faults to return typed `Io` set `allow_mmap_reads = false`.

Public documentation must state this contract beside the new default and the
copied-read opt-out.

## Benchmark and diagnostic contracts

### File-access controls

Change Modern benchmark/runner access modes to:

- `default`: production default, mapped where supported and available.
- `pread`: set `allow_mmap_reads = false`.

Remove the new-run `mmap` selector. Historical artifacts retain their recorded
mode and schema as raw evidence, but new tooling does not accept them as input.

Pinned LevelDB keeps its existing `default|pread` control, so the final
ADR-0053 measurement can compare:

```text
Modern default mmap vs LevelDB default mmap
Modern pread        vs LevelDB pread
```

Mutable workloads do not accept a Modern file-access override. The parity
matrix uses read-family cases only.

### Read-diagnostic schema 4

Bump new read-diagnostic reports from schema 3 to schema 4.

Schema 4 means:

- Lazy checked block decoding from schema 3.
- Production-default mmap with an explicit `pread` control.
- A count-only mapping budget.
- No `byte_budget_exhausted` setup reason.
- `validation_entries == 0` during the measured Get epoch.

New runners, validators, and reports support schema 4 only. Remove the
schema-2 historical-validation path added by ADR-0055 and remove the obsolete
`compare_read_diagnostics.py` experiment tool, its comparison helpers, and
their tests. ADR-0050 and ADR-0055 retain the historical outcomes; their raw
artifacts do not require an executable migration layer.

Do not add cross-schema comparison. Milestone 3 has no performance vote, and
the final ADR-0053 gate creates fresh reports from the completed path.

The frozen diagnostic corpus is compressed, so mapped stored blocks remain
owned after decompression and cacheable. Targeted uncompressed-table tests,
not a new diagnostic counter, prove borrowed cache bypass.

## Documentation updates

The implementation PR updates:

- ADR-0011: mmap is the POSIX default; `pread` is the explicit opt-out; the
  limiter is count-only.
- ADR-0025: decoded storage may be owned or borrowed; mapped uncompressed data
  bypasses the block cache.
- ADR-0026: block-cache entries remain independent of table mappings because
  borrowed blocks never enter the cache.
- ADR-0050: its accepted opt-in/copying experiment is superseded by this
  parity decision while its measured evidence remains historical.
- README and public option comments: single-process ownership, no external
  live-file modification, the independent mapped-page storage-fault risk,
  default mmap, and `allow_mmap_reads = false` for typed copied-read errors.
- Profiling documentation and help: Modern `default|pread` controls and
  diagnostic schema 4.
- Remove obsolete schema-2/3 validation and diagnostic-comparison
  documentation.

## Validation plan

### Block-format and block tests

- Owning uncompressed decode reuses the input allocation and is cacheable.
- Borrowed uncompressed decode aliases the input contents without the trailer
  and is not cacheable.
- Owning and borrowed Snappy/Zstd inputs produce owned cacheable decoded bytes.
- Both paths reject every trailer, checksum, type, and decompression error.
- Owned and borrowed empty decoded contents remain distinguishable.
- Moving `BlockContents` and `Block` preserves data and iterator validity.

### Filter and table tests

- Mapped index, metaindex, and filter bytes remain valid for the table
  lifetime without copies.
- A mapped uncompressed data block is never inserted into the block cache,
  including with `fill_cache = true`.
- Repeated reads of that block repeat the mapped decode and leave cache charge
  unchanged.
- A copied uncompressed block is cacheable and survives table eviction.
- A compressed block read from a mapping is cacheable, survives table
  eviction/unmapping, and is charged by decoded bytes.
- `fill_cache = false` uses existing entries but inserts neither ownership
  form.
- A table-cache handle keeps a mapping alive for `Get` and iteration.
- A borrowed data block never outlives its table; direct table iterators retain
  the documented table-outlives-iterator precondition.
- Mapped and copied modes return identical values, misses, deletions, iterator
  order, corruption, and short-read behavior.

### POSIX and public-option tests

- The default `PosixFileSystem` maps an exact-size nonempty file on 64-bit.
- The false boolean constructor and public false option force `pread`.
- The shared 1,000-count production policy and injected small count limits
  fall back and release slots correctly.
- No byte-budget fallback remains.
- Missing hints, empty files, and mismatched sizes retain copied fallback
  behavior.
- A mapped file remains readable after unlink.
- Concurrent mapped reads remain safe.
- Public defaults and documentation report mmap enabled.

Do not truncate a live mapping in-process to test `SIGBUS`.

The following branches retain ADR-0050's narrow justified coverage
exclusions and require direct code review rather than a new syscall-mocking
layer:

- Expected sizes that fit `uint64_t` but not 32-bit `size_t`.
- `fstat` failure after this implementation's private `open` succeeds.
- Resource-dependent real `mmap` failure.
- Reservation cleanup after `mmap` or allocation failure.

The reviewed fallback code must keep the opened descriptor and release any
acquired mapping slot before returning the copied file. Do not claim that
ordinary POSIX unit tests inject these conditions.

### Tooling and repository gates

- Benchmark and diagnostic parsers accept only the new controls and schema 4.
- Schema-4 reports require mapped default setup, copied `pread` setup, no byte
  fallback reason, and zero measured validation entries.
- Full native unit/public API tests.
- ASan/UBSan and TSan.
- LevelDB compatibility, model, and crash suites.
- Format and database fuzz smoke.
- AppleClang Release and GCC warning-clean builds.
- 100% changed-code line/branch coverage.
- Bounded GPT-5.6 Sol implementation review.

This milestone receives no standalone throughput admission test.

## Sequential implementation order

After this design PR is reviewed and merged:

1. Add `BlockContents` and convert stored-block decode tests.
2. Convert `Block`, `FilterBlockReader`, and table reads to preserve ownership.
3. Gate block-cache insertion on cacheability and add mapped lifetime tests.
4. Replace the POSIX count/byte budget with the count-only default policy.
5. Change public defaults, benchmark controls, diagnostics, and documentation.
6. Run the complete Milestone 3 validation and implementation review.
7. Deliver one implementation PR and merge it before Milestone 4 design.

Do not implement multiple steps on parallel branches.

## Rejected alternatives

### Keep copying mapped uncompressed blocks

Rejected. It preserves the exact double-caching divergence that Milestone 3
exists to remove.

### Cache a borrowed block together with a table or mapping pin

Rejected. Pinned LevelDB avoids this ownership edge entirely. It would add a
new cross-cache lifetime, extra reference traffic, and eviction coupling.

### Store a raw ownership flag beside an unconditional byte view

Rejected. A move-only RAII storage type makes ownership and cacheability
inseparable and avoids manual deletion or dangling-view states.

### Retain the 4-GiB production byte budget

Rejected. It changes which files map relative to the pinned count-based
baseline and is not required under the accepted 64-bit address-space and
exclusive-file-ownership contract.

### Add a new public file-access enum

Rejected. The existing boolean already represents the only supported product
decision: default mapped reads or an explicit copied-read opt-out.

### Retain schema-2/3 validators and cross-schema comparison

Rejected. There are no deployed diagnostic consumers, and the final parity
gate creates fresh schema-4 reports. ADR outcomes preserve the historical
evidence without permanent migration code.

### Recreate pinned heap iterators and callback-based `InternalGet`

Rejected. Current stack iterators and typed lookup results implement the same
control flow with fewer allocations. Reusable output belongs to Milestone 4.

### Map files supplied by a custom filesystem

Rejected. `TryReadView` is already the capability boundary. A custom
filesystem owns its storage policy and may expose stable views independently.

### Add a Milestone-3 performance vote

Rejected by ADR-0053. Performance is measured only after every parity
milestone is integrated.

## Consequences

- Default 64-bit POSIX databases use mapped immutable table files and accept
  the documented external-mutation and independent mapped-storage-fault
  behavior.
- Applications can force copied reads with the existing public boolean.
- Mapped uncompressed blocks stop consuming both mapping and block-cache
  storage.
- Copied and compressed blocks retain independent cache lifetimes.
- Table member order becomes an explicit borrowed-storage invariant.
- Diagnostic tooling has one current schema and no historical compatibility
  path.
- Remaining read-path ownership and traversal differences are isolated to
  ADR-0053 Milestone 4.

## References

- [ADR-0011 filesystem contracts](0011-filesystem-contracts.md)
- [ADR-0025 SSTable reader](0025-sstable-reader.md)
- [ADR-0026 table cache](0026-table-cache.md)
- [ADR-0050 mmap experiment](0050-posix-mmap-table-reads.md)
- [ADR-0053 complete read-path parity](0053-leveldb-read-path-parity.md)
- [ADR-0055 block-iterator parity](0055-leveldb-block-iterator-parity.md)
- [Pinned LevelDB table reader](https://github.com/google/leveldb/blob/7ee830d02b623e8ffe0b95d59a74db1e58da04c5/table/table.cc)
- [Pinned LevelDB stored-block reader](https://github.com/google/leveldb/blob/7ee830d02b623e8ffe0b95d59a74db1e58da04c5/table/format.cc)
- [Pinned LevelDB POSIX mmap limiter](https://github.com/google/leveldb/blob/7ee830d02b623e8ffe0b95d59a74db1e58da04c5/util/env_posix.cc)
