# ADR-0092: Native Write Cost-Model Alignment

- Status: Accepted

## Context

[ADR-0060](0060-leveldb-write-path-parity.md) aligned the write pipeline's
behavior and major mechanisms. It did not establish instruction-level or
universal workload equivalence. Its retained `mixed50` regression compares
two historical Modern versions, not Modern against Native LevelDB.

This follow-up uses Modern revision
`38815a431b339f2d085cf41454f26e4685e916a0` and pinned Google LevelDB
`7ee830d02b623e8ffe0b95d59a74db1e58da04c5`. The current runtime trees match
the unchanged Release archive retained for ADR-0091:

- `src`: `9869858df64f217b7501abe7fe561ca607cc7ffb`;
- `include`: `a426540434bb14f248c255ef8549e83ed59ee9f5`.

Native's CRC wrapper reaches the same frozen Google CRC32C provider as
Modern. This is not a comparison against a portable-checksum reference.

### Preserve the existing cheaper mechanisms

Three measured or directly observable differences are not reasons to copy
Native's source literally:

- With a fresh 16-byte-key/100-byte-value batch, Modern made one C++
  allocation per record construction; Native made two. Both reused batch
  capacities without allocation.
- The existing cross-level upper-bound comparison cache applies to
  insertion as well as lookup. A deterministic 262,144-insertion overwrite
  trace required 7,323,058 user comparisons in Modern and 7,970,721 in
  Native, with identical arena reservations. Ascending insertion had equal
  comparison counts.
- Modern flushes the writable buffer after the complete logical WAL record.
  Native flushes each physical fragment. The current complete-record
  visibility and durability contract does not require copying the extra
  fragment flushes.

`WriteOptions::sync=false` is already the per-write default in both engines.
It does not make either write API fire-and-forget. Modern's separate
WAL-creation namespace policy remains a deliberate stronger default.

### Remaining generated work

The actual optimized Modern callers still contain:

- outlined varint length/encoding and batch-header word access;
- a separately returned 56-byte optional entry for each batch record;
- optional fragment transport plus a separate status-returning physical
  fragment envelope;
- outlined empty queue-guard cleanup and pointer/sequence accessors;
- three separate vector-header append operations per SSTable entry.

The 72-byte expected-based Status and Native's 8-byte Status are different
representations. Size alone is not a cycle count or an allocation claim.
The allocation-counting image observed no per-record heap allocation in
batch iteration or WAL framing.

Thirty-six fixed-work component processes ran serially in three balanced
rounds, without concurrent builds or adaptive calibration. Ordinary and
allocation-counting images were separate. Every byte/order oracle and all
arena accounting matched. The isolated batch parser, null-sink WAL writer,
and block producer were slower in all three paired rounds; fresh/reused
batch encoding and MemTable insertion were faster in all three.

These measurements identify work to investigate. The null sink is not disk
I/O, parser time is not complete batch latency, and the component ratios
cannot be summed into a predicted write speedup.

### Repair the current profiling fixture before comparing whole writes

The current profiling adapter inherited Modern's newer enabled-filter
default while its context still declared `filter_policy=none`. Native's
adapter had no filter. That configuration can charge filter construction
or exclusion benefits to an alleged core write/read difference.

Use shared option factories for database opening and reported filter state.
Explicitly disable Modern filters in the matched fixture and require
`filter_configuration_semantics=shared-open-options-v1` in the runner.
Missing or different semantics are unverified, not a successful default.
This does not change the library's production filter policy.

An executable regression first reported `enabled` for Modern and `none`
for Native. The parser regression initially demonstrated acceptance of
missing/mismatched information; the repaired runner rejects it.
Historical ADR-0060's revision
`e0723011c60e1e4cffa6a8b16ed1fa48beeb0480` had a disabled optional filter
default, so this newer-default mismatch does not invalidate its retained
original configuration.

## Decision

Use Native as the behavioral and cost-model baseline while retaining
compatible mechanisms that already do less work. Apply architecture-
compatible RocksDB producer techniques in the same bounded composition,
rather than first reproducing an avoidable Native cost.

RocksDB reference revision:
`553b52b4a709820fac1dbadc7796506740517582`.
Its block builder uses the variadic `PutVarint32` helper to append the three
length fields together; its coding helpers operate on advancing output
pointers. This is a producer implementation choice, not permission to
adopt a different RocksDB table or WAL format.

A bounded independent design review approved the five mechanisms below
without a correctness blocker. It verified the prefix/exception premises,
cursor advancement, I/O ordering, guard scopes, and predecessor invariant.
That approval is not implementation or performance evidence.

### 1. Compiler-visible trusted producer codecs

Keep external checked functions and public types unchanged. Put the shared
trusted varint encoder/length calculation and internal-key writer in
private headers. Advance a local output pointer and update the span once
after encoding, instead of repeatedly materializing span updates.

Make the private batch header getters/setters visible to their consumers,
using the established endian-safe complete-word helpers. Preserve a single
codec implementation rather than creating a parallel encoding dialect.

Preserve the one-reservation record construction. Use C++23
`resize_and_overwrite` to initialize the appended record directly without
first clearing bytes that the encoder immediately replaces. Input views
must not alias this private encoding, as already required by the owned-batch
contract. The callback does not throw or inspect the string object while
the operation owns its buffer.

### 2. Direct consumption of validated batch entries

Add a private-header `ForEachRemaining` operation to WriteBatchReader.
It consumes the same validated/owned cursor through one shared entry codec,
with a compiler-visible consumer. Production insertion no longer needs the
optional return object or an outlined Next call for each entry.

Retain Next for explicit stepping. Both operations:

- preserve sequence, kind, byte views, order, and end behavior;
- borrow unchanged storage protected by the reader's existing contract;
- advance before invoking a potentially throwing consumer, matching the
  previous Next-then-insert exception position;
- leave later records available after a consumer throws.

External Open still validates the entire structure before either operation.
Neither an unknown tag nor truncated external bytes enter a new trusted path.

### 3. Scalar WAL cursor completion and direct emission

Add a boolean cursor step that fills a caller-owned fragment. Keep the
optional step as a compatibility wrapper over that same algorithm.
False means exhausted and leaves the output unchanged; it is not an I/O
error or a recoverable parsing result.

Use scalar stepping in WalWriter and emit padding, header, and payload
directly in the record loop. Remove the redundant status-returning fragment
envelope, not error checks. Preserve the first failing operation, no flush
after an append failure, exactly one successful logical-record Flush,
explicit Sync, reopened-block padding, and the historical empty-First case.

### 4. Fused SSTable header construction

Encode the three existing uint32 varints into at most fifteen stack bytes
and append their initialized prefix once. This borrows RocksDB's grouped
header mechanism while keeping the typed byte buffer, existing payload
insertion, restart policy, estimates, and exact compatible encoding.

### 5. Visible ownership envelopes and established predecessors

Make the tiny queue guard and active-buffer/sequence accessor bodies
compiler-visible. Substantial completion, rotation, and error handling stay
outlined. Normal dismissal can then eliminate empty cleanup; exceptional
unwinding must still reacquire the mutex, restore the hidden batch sequence,
complete the correct group, and preserve stack-writer lifetimes.

Do not zero all twelve predecessor pointers before insertion. Search fills
every active level and height growth fills each newly needed level before
linking. This is the same established predecessor invariant used by Native.
Keep the existing explicit construction of trailing atomic links; removing
their object-lifetime establishment is not part of this change.

## Unchanged boundaries

- No public signature, Result representation, file format, or default policy
  change.
- No weaker WAL creation, write sync, checksum, exception, sticky-error,
  queue-group, or recovery guarantee.
- No speculative parallel MemTable insertion, pipelined writes, new cache
  policy, or scheduling architecture.
- No claim that an already faster component needs to imitate slower code.

The remaining larger typed I/O return representation is a named residual
cost, not evidence that exceptions or safety must be disabled.

## Verification and evaluation

Use test-first bounded increments and retain small topic commits:

1. literal varint/header bytes, full-width boundaries, output sentinels,
   batch prefix preservation, reuse/copy/move behavior;
2. checked and owned entry visitation, partial consumption, binary/empty
   values, end behavior, and a throwing consumer's exact cursor position;
3. both cursor APIs across all header/block/payload boundaries, unchanged
   output at exhaustion, complete WAL bytes, and each injected I/O failure;
4. fused multi-byte entry headers, restart boundaries, golden blocks,
   table compatibility, and shared-codec consumers;
5. queue success/failure/exception/grouping, rotation, memory publication,
   randomized skip-list insertion and concurrent-reader safety.

Run the existing compatibility, crash, sanitizer, and ordinary CI gates.
Do not relax coverage or change-routing contracts.

For cost evidence, rebuild every affected Release translation unit using
the retained options and checksum/compression providers, with matching
headers. Keep the baseline and Native images unchanged. Inspect actual
optimized callers, not unused template bodies.

Measure the completed composition with serial fresh fixed-work processes,
matched policies, retained order/raw results and output verification.
Include overwrite, batch, sync, and mixed read/write behavior; distinguish
foreground acknowledgement from background drain. Component observations
remain diagnostic and are not a preset numeric acceptance threshold.

## Sources

- [Pinned Native write pipeline](https://github.com/google/leveldb/blob/7ee830d02b623e8ffe0b95d59a74db1e58da04c5/db/db_impl.cc)
- [Pinned Native batch implementation](https://github.com/google/leveldb/blob/7ee830d02b623e8ffe0b95d59a74db1e58da04c5/db/write_batch.cc)
- [Pinned Native WAL writer](https://github.com/google/leveldb/blob/7ee830d02b623e8ffe0b95d59a74db1e58da04c5/db/log_writer.cc)
- [Pinned RocksDB coding helpers](https://github.com/facebook/rocksdb/blob/553b52b4a709820fac1dbadc7796506740517582/util/coding.h)
- [Pinned RocksDB block producer](https://github.com/facebook/rocksdb/blob/553b52b4a709820fac1dbadc7796506740517582/table/block_based/block_builder.cc)
- [Behavioral write parity](0060-leveldb-write-path-parity.md)
- [Compiler-visible read ownership and codecs](0091-native-get-cost-model-parity.md)
