# ADR-0055: Pinned LevelDB Block-Iterator Parity

## Status

Accepted design. Design PR #67 completed bounded GPT-5.6 Sol review and
merged before this ADR-0053 Milestone 2 implementation began.

## Context

ADR-0053 requires the complete pinned Google LevelDB read path before any
further isolated optimization experiments. ADR-0054 completed the cache
ownership milestone. The next layer is the block iterator and its error
propagation through tables, merged iterators, database iterators, and
compaction.

Current Modern blocks establish a stronger boundary than pinned LevelDB:

- `Block::Create` scans every encoded entry and restart point.
- The scan reconstructs keys for order validation, except that ADR-0049 trusts
  data-block key order.
- ADR-0046 then uses an unchecked decoder in every iterator move.
- `Block` retains its comparator, so cached blocks retain read-policy state.
- The iterator reconstructs keys in `vector<byte>`, which has no
  `std::string`-style short-string storage.
- Iterator moves cannot report corruption because construction promised all
  entries were valid.

Pinned LevelDB instead validates only the restart-array bounds at block
construction. `Block::Iter` checks entry headers, extents, prefix lengths, and
restart entries as it reaches them; keeps a status; reconstructs its key in
`std::string`; and receives the comparator when the iterator is created.

This is a cohesive mechanism. Lazy decoding without status propagation is
unsafe. Direct comparison without a checked minimum internal-key length is
unsafe. `std::string` storage without removing the eager scan does not produce
the same work placement. The milestone implements the complete composition
without a per-piece performance vote.

## Scope

This ADR covers:

- `Block` construction and immutable layout.
- `Block::Iterator` decoding, seeking, key reconstruction, and error state.
- The table-only direct internal-key comparator boundary.
- Status propagation through `Table::Get`, `Table::Iterator`,
  `LevelIterator`, `MergingIterator`, `DbIterator`, and compaction.
- Tests, fuzzing, and read-diagnostic contracts directly affected by moving
  structural validation from load time to iterator moves.

It does not cover:

- Borrowed mmap-backed uncompressed `Block` ownership or cacheability, which
  is ADR-0053 Milestone 3.
- Default mmap policy, also Milestone 3.
- Memtable/version intrusive read pins, lazy point-read file visitation, seek
  charging, or reusable public output, which are Milestone 4.
- Broader trusted comparison in memtables, merging iterators, file metadata,
  or compaction bookkeeping.

## Pinned block mapping

| Pinned `Block` / `Iter` field | Modern field | Decision |
|---|---|---|
| `data_` | owning `vector<byte>` plus borrowed `ByteView` | Ownership changes wait for Milestone 3 |
| `size_` error marker | successful `Result<Block>` after restart-region checks | Preserve typed early construction error |
| `restart_offset_` | `std::size_t entries_end` | Same boundary without 32-bit truncation |
| `num_restarts_` | `std::size_t restart_count` | Same count |
| iterator comparator | `const Comparator* comparator` | Borrowed by each iterator, not retained by `Block` |
| `current_` | `std::size_t current` | Same valid/invalid boundary |
| `restart_index_` | `std::size_t restart_index` | Same role |
| `key_` `std::string` | `std::string key` | Same binary-safe storage and SSO behavior per standard library |
| `value_` `Slice` | `ByteView value` | Same borrowed lifetime |
| `status_` | optional `Error` plus per-move `Status` | Preserve Modern's explicit error-return API |

## Block construction

Replace both current factories with:

```cpp
class Block {
 public:
  static Result<Block> Create(std::vector<std::byte> contents);
};
```

`Create` performs only the safe restart-region checks needed before an
iterator can read the restart array:

1. The contents contain the fixed32 restart count.
2. The count fits in the bytes before itself:

   ```text
   count <= (size - sizeof(fixed32)) / sizeof(fixed32)
   ```

3. Compute the entry/restart boundary without overflow.

As pinned LevelDB does, a zero restart count is an empty block rather than a
construction error. An empty builder block has one restart at zero and is also
empty.

`Block::empty()` is table-oriented and reports that no position is reachable:

- The restart count is zero.
- The entry region is zero.
- The first restart offset is at or beyond the entry boundary.

Reading the first restart is safe after the restart-region count check. This
lets the existing table invariant reject a data block that contains unreachable
bytes instead of reporting a normal miss.

Do not decode entries, inspect individual restart offsets, reconstruct keys,
compare order, or retain a comparator at construction. Remove
`CreateWithTrustedKeyOrder`, `Validate`, `ValidateStructure`, and the trusted
validated decoder.

Modern retains one explicit timing exception: a too-short block or a restart
count that does not fit returns `Corruption` from `Create` rather than creating
LevelDB's size-zero error-marker block. The same malformed bytes remain
rejected before unsafe access, and valid-path work is identical.

## Checked entry decoder

Implement the pinned `DecodeEntry` shape privately in `block.cc`:

```cpp
const std::byte* DecodeEntry(
    const std::byte* input, const std::byte* limit,
    std::uint32_t& shared, std::uint32_t& non_shared,
    std::uint32_t& value_size) noexcept;
```

- Require at least three bytes for the one-byte fast path.
- When all three first bytes are below 128, consume them directly.
- Otherwise decode each varint32 with at most five bytes and no read past
  `limit`.
- Match `ConsumeVarint32` and pinned LevelDB semantics for nonminimal
  encodings and excess fifth-byte payload bits.
- Check key-delta plus value extents with `std::size_t`/64-bit arithmetic so
  the two uint32 lengths cannot wrap.
- Return `nullptr` for any malformed header or extent.

The decoder never forms a pointer beyond the entry region. Restart offsets are
checked against the entry boundary before pointer arithmetic.

## Iterator contract

Add an internal key-format selector:

```cpp
enum class BlockKeyFormat {
  Arbitrary,
  Internal,
};

class Block::Iterator {
 public:
  Iterator(const Block& block, const Comparator& comparator,
           BlockKeyFormat format = BlockKeyFormat::Arbitrary) noexcept;

  Status SeekToFirst();
  Status SeekToLast();
  Status Seek(ByteView target);
  Status Next();
  Status Prev();
};
```

The block and comparator must outlive the iterator. Deleted overloads reject a
temporary block or comparator. Cached blocks retain bytes and restart layout
only.

### Error and recovery

The iterator stores the first corruption from its current positioning
attempt, invalidates its position, clears its key/value, and returns that
error. `Next` and `Prev` require a valid iterator.

`Seek`, `SeekToFirst`, and `SeekToLast` clear the prior error and start over.
This retains Modern's documented recovery contract while LevelDB's public
iterator keeps a sticky `status()`.

Allocation failure remains an exception. Reserve the required reconstructed
key capacity before changing the current offset/key/value so a `bad_alloc`
leaves a consistent previous or invalid position.

### Restart access

Reading a restart:

- Requires an index below `restart_count`.
- Reads fixed32 only from the bounded restart array.
- Rejects an offset greater than the entry boundary before forming an entry
  pointer.

Binary-search restart entries must decode successfully, have `shared == 0`,
and meet the selected key-format boundary.

### Key reconstruction

Use `std::string` exactly as pinned LevelDB does:

- Resize to the shared prefix.
- Append arbitrary binary delta bytes.
- Expose the bytes as `ByteView`.

This supersedes ADR-0047's rejection of isolated inline storage. Here string
storage is a required component of the complete pinned block mechanism rather
than an independently admitted point-read optimization.

### Positioning algorithms

Adopt the pinned algorithms exactly:

- `Seek` narrows restart search from the current position when possible.
- Seeking the current key returns without moving.
- Restart binary search chooses the last restart whose key is less than the
  target.
- Linear search parses until the first key not less than the target.
- `Prev` scans to the restart before the original entry and rebuilds forward.
- Restart-index advancement uses pinned LevelDB's strict
  `next_restart_offset < current_offset`.

A normal end invalidates without error. A malformed reached entry returns
`Corruption("bad entry in block")`.

Every first/last/seek path handles `restart_count == 0` before subtracting one
from the count or reading a restart.

## Internal-key comparison boundary

Keep `InternalKeyComparator::Compare(ByteView, ByteView)` unchanged for
arbitrary callers and malformed-key deterministic ordering.

Add:

```cpp
int InternalKeyComparator::CompareTrusted(
    ByteView left, ByteView right) const noexcept;
```

It requires both operands to contain the eight-byte internal-key trailer,
asserts that precondition in debug builds, compares user keys directly, and
then compares decoded fixed64 trailers in descending order. It does not
validate value kinds.

Add a private/internal `TrustedInternalKeyComparator` adapter implementing the
generic `Comparator` interface. Its shortening functions forward to the
defensive comparator and are unused by block iteration.

`BlockKeyFormat::Internal` establishes memory safety before every comparison:

- A raw seek target shorter than eight bytes is corruption.
- Every decoded restart or reconstructed key shorter than eight bytes is
  corruption.

Unknown value kinds are safe to compare but remain unsupported semantics.
Consumers return corruption when they parse an encountered key.

## Table boundary and direct-safe mode

Extend internal `TableOptions` with:

```cpp
bool use_trusted_internal_key_comparison = false;
```

`DatabaseEngine` sets it to true for every production table. Direct table
tests/callers may leave it false, retaining defensive comparison for
arbitrary-key LevelDB golden tables.

`Table` owns one `TrustedInternalKeyComparator` beside its defensive
`InternalKeyComparator` reference. It selects:

- Trusted comparator plus `BlockKeyFormat::Internal` in production mode.
- Defensive comparator plus `BlockKeyFormat::Arbitrary` in direct-safe mode.

The adapter belongs to `Table`, not `Block`, so a block-cache entry retains no
comparator pointer.

### Table open

- Index/metaindex blocks use the new minimal `Block::Create`.
- A table-only full-entry validator walks index and metaindex bytes physically
  from offset zero, independent of the restart array. It uses the checked
  decoder and reconstructs each key.
- That validator requires the first restart to be zero; restart offsets to be
  strictly increasing; and every restart to identify a decoded entry boundary
  whose `shared` length is zero. Every restart in a nonempty entry region must
  be consumed exactly once; an empty block retains its sole restart at zero.
- `ValidateIndex` therefore visits every physical index entry, parses every
  key in trusted mode, validates every exact handle/range/non-overlap value,
  and cannot leave an unchecked entry reachable through restart binary search.
- Metaindex validation likewise covers every physical entry before filter
  lookup.
- Filter lookup propagates block-iterator corruption.

The validator does not compare key order. Index, metaindex, and data key order
remain writer-established format invariants, matching pinned LevelDB.

### Point lookup

`Table::Get`:

1. Propagates index seek corruption.
2. Loads/filter-checks the selected data block.
3. Propagates data seek corruption.
4. Parses the found internal key explicitly.
5. Returns corruption for a short key or unsupported value kind.
6. Compares the parsed user key and copies the result as before; reusable
   output belongs to Milestone 4.

No `.value()` assumes a parse that a defensive comparator happened to prove.

### Table iteration

Every index/data block move returns `Status`. `Table::Iterator` propagates it,
clears its current block/data iterator on failure, and remains recoverable by a
later positioning call.

In trusted mode, `Table::Iterator::Seek` parses the raw internal target before
the first trusted comparison. `SeekToFirst`, `SeekToLast`, `Next`, and `Prev`
rely on the per-entry minimum-length check and let higher layers parse semantic
kinds when encountered.

## Higher-layer propagation

The existing `InternalIterator` interface already returns `Status` from every
move. Preserve its contract:

- `LevelIterator` closes the current table on a table-iterator failure and
  returns it.
- `MergingIterator` invalidates and returns the first child failure.
- `DbIterator` invalidates and returns block corruption or an unsupported
  internal key encountered in either direction.
- `RunCompaction` stops before writing the corrupt entry and returns the input
  error.

Add real malformed-table tests, not only scripted iterator failures, at each
layer. A later positioning call starts over where the existing contract says
it does.

Memtable iterators and builder iteration remain unchanged.

## Diagnostic contract

Eager construction no longer visits entries. Keep the
`validation_entries` counter for schema compatibility but require it to remain
zero during the fixed measured epoch. Index/metaindex full validation happens
during excluded setup/table open, not the measured Get epoch.

Bump the read-diagnostic report schema from 2 to 3 and update validators/tests:

- Schema 3 means lazy checked block decoding.
- `index_entries_decoded`, `data_entries_decoded`, and
  `restart_entries_decoded` continue to count successful iterator decodes.
- `internal_key_comparisons` includes defensive and trusted table comparisons.
- Remove the old invariant that every decoded block has construction-time
  validation entries.
- Schema 3 is the default and is mandatory for every new candidate/report.
- Schema 2 is accepted only when the caller explicitly selects historical
  schema-2 validation; it retains the old construction-validation invariant.
- `compare_read_diagnostics.py` rejects a schema-2 input unless
  `--historical-schema2-baseline` is present, and never permits a schema-2
  candidate.
- A schema-2 baseline versus schema-3 candidate validates both reports under
  their own contracts, compares the other frozen work counters, and records
  `validation_entries` as intentionally incomparable rather than applying the
  2% drift rule.
- Schema-3 versus schema-3 comparison may include `validation_entries`; both
  sides must report zero.

`BlockConstruction` timing remains the allocation/layout construction stage
and may still have cache-pressure events, but it no longer includes an entry
scan.

## API and caller changes

Update every direct block caller:

- Table index, metaindex, and data iterators.
- Table builder/reader tests.
- Block unit/model tests.
- Format fuzzer.

All iterator moves must check returned status. No caller may ignore a
`[[nodiscard]]` corruption result.

Remove tests that require block creation to reject malformed entries or
unordered keys. Replace them with:

- Construction acceptance after valid restart-region bounds.
- Corruption at the exact iterator operation that reaches a malformed header,
  extent, shared prefix, restart offset, or restart entry.
- Recovery by a later positioning call.
- Writer and ordered-model tests that continue to prove valid block order.

## Approved deviations from pinned LevelDB

No other deviation may be introduced without amending and reviewing this ADR.

| Pinned behavior | Approved Modern behavior | Reason |
|---|---|---|
| Invalid restart region becomes a size-zero block and error iterator | `Block::Create` returns typed corruption | Same valid-path work, earlier explicit error |
| Potential pointer formation from an out-of-range restart offset | Check offset before pointer arithmetic | Memory safety |
| uint32 key/value extent addition | checked `size_t`/64-bit extent arithmetic | Prevent wraparound and overread |
| Sticky iterator `status()` | per-move `Status`; positioning calls recover | Existing repository-wide iterator API |
| `std::string` exceptions are not handled | reserve before state mutation; `bad_alloc` propagates | Existing exception-consistency contract |
| Direct unchecked comparator everywhere | direct compare only after minimum-length boundary | Preserve arbitrary-input safety |
| All tables assumed internal-key tables | direct-safe defensive option for internal tests/tools | Preserve existing plain-key golden-table coverage |
| Empty data block is skipped | data block with no positions is corruption | Existing Modern table/version invariant |
| Malformed index discovered on use | complete index walk at table open | Existing one-time handle/range validation |

## Validation plan

### Block unit/model tests

- Restart count zero, empty builder block, and valid restart-region
  construction.
- A nonempty entry region with zero restarts or a first restart at/beyond the
  entry boundary has no positions; table data loading rejects it as empty.
- One-byte fast headers and one- through five-byte varints, including
  nonminimal and excess fifth-byte payload encodings.
- Truncated/unterminated headers, overflowing extents, invalid shared prefixes,
  restart offsets outside the entry region, and restart entries with nonzero
  shared length.
- Corruption from first/last/seek/next/prev and recovery on the next
  positioning call.
- Binary keys/values, growing and shrinking keys, restart-boundary direction
  changes, current-position seek narrowing, reverse comparator, and block move
  lifetime.
- Ordered randomized model across restart intervals.

### Comparator and table tests

- Randomized trusted/defensive sign equivalence for valid internal keys and
  custom user comparators.
- Defensive malformed-key total order remains unchanged.
- Debug assertions reject direct short trusted operands.
- Table open rejects a malformed index entry/key before trusted use.
- Index/metaindex open rejects a first restart other than zero, duplicate or
  descending restarts, a restart into an entry payload, a restart without
  `shared == 0`, and any physical entry/key/handle omitted by a malicious
  restart layout.
- Lazy data corruption surfaces through Get and every iterator direction.
- Short/unknown-kind data keys and short/invalid seek targets return
  corruption.
- Direct-safe plain-key golden tables remain readable.
- Cached blocks retain no comparator pointer and may outlive a table cache
  mapping while pinned.

### Higher-layer tests

- Real malformed table data propagates through level, merging, DB iterator,
  and compaction paths.
- Every failure invalidates the exposed iterator and a later positioning call
  recovers where specified.
- Valid LevelDB compatibility/model/compaction output remains unchanged.

### Repository gates

Run:

- Full native unit/public API tests.
- ASan/UBSan and TSan.
- LevelDB compatibility/model/crash suites.
- Format fuzz smoke.
- AppleClang Debug/Release and GCC warning-clean builds.
- Changed-code line/branch coverage at 100%.
- Optimized-code inspection proving trusted table comparison does not call the
  defensive decoder.

Do not run a block-only throughput admission test. Performance is measured
only after every ADR-0053 milestone is complete.

## Rejected alternatives

### Retain eager validation plus `std::string`

Rejected. It mixes Modern's old work placement with one LevelDB storage detail
and repeats the isolated-experiment mistake.

### Keep unchecked decoding after construction

Rejected. Once eager entry validation is removed, checked lazy decoding is the
memory-safety boundary.

### Make the general internal comparator unchecked

Rejected. Direct tests, memtable seeks, merging targets, metadata, and other
callers still accept byte views without this milestone's checked boundary.

### Store comparator policy in cached blocks

Rejected. The iterator borrows its comparator; the immutable block retains
only bytes and restart layout.

### Return a sticky `status()` instead of per-move `Status`

Rejected. It would rewrite every Modern iterator API without improving parity
work or ownership.

## Consequences

- Valid table reads move entry checks from every block-cache miss to entries
  actually visited by block seeks and iteration.
- The block key buffer and seek algorithm match the pinned implementation.
- Table block comparisons remove repeated valid-key parsing only after an
  explicit checked boundary.
- Corruption can now appear during iterator movement and is propagated through
  every existing typed status surface.
- Cached blocks retain no comparator or table lifetime.
- ADR-0046's validated unchecked decoder and ADR-0051's isolated rejection
  remain historical evidence but no longer describe production after this
  milestone.

## Delivery boundary

Obtain one bounded GPT-5.6 Sol review of this ADR against pinned
`table/block.cc`, `table/table.cc`, and every Modern caller listed above.
Resolve every material finding and merge the design-only PR. Only then create
a fresh Milestone-2 implementation branch.

## References

- [ADR-0022 SSTable block format](0022-sstable-block-format.md)
- [ADR-0025 SSTable reader](0025-sstable-reader.md)
- [ADR-0031 iterators](0031-iterators.md)
- [ADR-0035 running compactions](0035-running-compactions.md)
- [ADR-0046 validated block decoding experiment](0046-validated-block-decoding-experiment.md)
- [ADR-0047 inline key experiment](0047-inline-iterator-key-experiment.md)
- [ADR-0051 trusted comparison experiment](0051-trusted-internal-key-comparison.md)
- [ADR-0053 pinned LevelDB read-path parity](0053-leveldb-read-path-parity.md)
- [Pinned LevelDB block iterator](https://github.com/google/leveldb/blob/7ee830d02b623e8ffe0b95d59a74db1e58da04c5/table/block.cc)
- [Pinned LevelDB table lookup](https://github.com/google/leveldb/blob/7ee830d02b623e8ffe0b95d59a74db1e58da04c5/table/table.cc)
