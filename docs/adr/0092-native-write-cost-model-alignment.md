# ADR-0092: Native Write Cost-Model Alignment

- Status: Implemented

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
- Existing table construction also batches buffer draining instead of
  explicitly flushing after every data block. A separate baseline counter
  built 65,536 entries into byte-identical 7,528,491-byte uncompressed,
  filter-free tables: 3,589 Appends on both sides, zero explicit Modern
  Flush calls versus 1,792 Native Flush calls, and one Sync and Close on
  both complete lifecycles. These are interface counts, not a measured
  syscall ratio or a causal share of the complete write gap.

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

## Retained implementation and measurement outcome

### Frozen runtime identities

The implemented runtime revision is
`0ea06e906f5b877d7a405fd8f5989a1bd64fd762`, with:

- `src`: `868b5aad36c60815905589f7208c4ad7087cd954`;
- unchanged `include`: `a426540434bb14f248c255ef8549e83ed59ee9f5`;
- candidate archive SHA-256:
  `3e662cd4f93ca8d49fb89892d464c51625bf0639f5875da657dfd5bf1c612136`;
- candidate source archive SHA-256:
  `7f1e0e4a3fc7af92a54dbf8034a3d6cddf91231f514630a6a39fbd62ab18d60f`.

All sixty ordinary core translation units were rebuilt with the retained
Release options, changing only object/PDB destinations. The experiment used
the same frozen Native, CRC32C, Snappy, Zstd, and Google Benchmark archives.
Public headers remained unchanged; each workload used the headers matching
its role.

The Native archive is the unchanged prior hardware-CRC reference, not a
fresh rebuild of canonical disabled-CRC LevelDB. Its control-patch SHA was
not reconstructed for this follow-up; the generated headers explicitly
record that limitation. Exact prior archive identities and the shared
linked CRC provider were checked rather than inventing a patch hash.

Actual optimized callers show:

- no outlined varint-length/fixed-word calls or whole-record clearing in
  the batch record constructor and its resize callback;
- one batch-level visitor loop whose entry path calls MemTable directly,
  without per-entry Next/ConsumeEntry calls or optional result transport;
- no separate sequence/header/active-buffer accessor calls in CommitWrite;
- scalar WAL stepping and no status-returning WriteFragment envelope;
- grouped block header emission instead of three public varint appends.

Some caller bodies grow because formerly outlined work becomes visible.
Static instruction totals and Status size are not cycle savings.

### Complete 64-KiB fixture: keep every adverse observation

The existing fixed-work callback and corpus were unchanged. Each process
used one foreground caller, the actual process affinity mask 340 over four
selected physical cores, matched Snappy/checksum providers, and no adaptive
calibration or forced background drain. Three rotated, role-reversed rounds
contained all 72 fresh processes. Twenty verified smoke cells were excluded.

The matched roles disable filters, select exclusive batch ownership, and
use LevelDB-equivalent WAL creation. Separate production-policy controls
enable the current full filter, copying batch API, and default WAL-creation
policy under the same fixture knobs. On Windows that creation policy still
has the explicitly weak namespace contract; it is not a POSIX directory
durability guarantee.

Positive deltas below mean the composition is slower than its own before
image. The estimator is the median of three within-round cost ratios, not
the ratio of aggregate medians:

| Policy | Workload | Wall delta | Process-CPU delta | All wall round deltas |
|---|---|---:|---:|---|
| Matched | overwrite | +2.30% | +2.62% | +2.30%, +1.71%, +5.60% |
| Matched | writebatch | +1.04% | +0.59% | +5.68%, +1.04%, -2.20% |
| Matched | writesync | +9.93% | +27.27% | +9.93%, +19.24%, +5.94% |
| Matched | mixed50 | -1.76% | -1.80% | -3.47%, -1.76%, +0.12% |
| Production policy | overwrite | -2.10% | -4.01% | -2.10%, -2.03%, -8.01% |
| Production policy | writebatch | +2.72% | +2.48% | +4.90%, +0.75%, +2.72% |
| Production policy | writesync | -3.89% | 0.00% | +6.24%, -3.89%, -7.68% |
| Production policy | mixed50 | -0.59% | -4.01% | -0.59%, -4.84%, +0.56% |

The matched sync regression is a real observation in all three rounds,
not a speedup. Same-image Native A/A sync variation reached 14.70% in one
round; that is descriptive context, not a correction factor, confidence
bound, or proof that the regression is noise.

Final state is not uniformly less indebted: matched overwrite retained
median 6,001,039 table bytes before versus 10,035,883 after, and matched
batch retained 9,260,257 versus 9,859,908. All logical fingerprints matched,
but these runs do not establish equal completed background work or steady
state.

The full plan and raw-result SHA-256 values are:

- plan: `c41fbb21cbcebdc70b5fc9a5315a6b6ce00dff9602e199e2317abf0b275c8919`;
- results: `ec5b26f04ccd6995bdfa7ada407aac4bebd4c35b6323d0f9911a8370e435774b`;
- summary: `4c5bc68a1c963464f42d138992ca69f2f373595030b7093cb01a7c40349058c2`.

### Separately declared shared 4-MiB default-buffer control

The small fixture rotates and performs maintenance frequently. A follow-up
declared the actual shared 4-MiB write-buffer default, retaining exactly
the same core archives, logical work, loop, filters/ownership/checksum
roles, affinity, and acknowledgement boundary. Effective metadata was
derived from the modified option factories. This is a different
configuration, not a replacement, rerun, or pool of the 72-cell matrix.

All 48 fresh processes in three case-rotated, alternating-role rounds
completed, with twelve
excluded smoke cells:

| Workload | Wall delta | Process-CPU delta | All wall round deltas |
|---|---:|---:|---|
| overwrite | -1.79% | 0.00% | -1.79%, -3.40%, +6.44% |
| writebatch | -2.60% | -6.78% | +3.71%, -2.60%, -4.45% |
| writesync | +2.32% | +50.00% | +2.33%, +2.32%, -1.11% |
| mixed50 | +1.20% | -0.36% | +1.64%, +1.20%, +1.20% |

The mixed regression remains in every round. Sync CPU ratios were
particularly coarse and unstable; the +50% median is not a precise
mechanism-cost estimate. Native A/A wall variation reached 13.07% for
sync and 9.74% for batch in individual rounds. Do not subtract it or infer
formal equivalence from small deltas.

Relative to the predeclared mean of the two Native wall slots per round,
the candidate's overwrite deltas ranged from -8.18% to -2.88%, batch from
-19.79% to -17.69%, sync from -4.43% to +2.34%, and mixed from -3.15% to
-0.74%. These observations depend on the stated configuration and do not
assign a causal fraction to allocation, comparisons, or buffer draining.

### Engineering interpretation

The composition removes the identified generated work and preserves the
named boundaries. It does **not** demonstrate a consistent whole-write
speedup. Keep the sync and mixed regressions, configuration dependence,
Native A/A variation, and residual-state differences visible.

A bounded implementation review found no significant issue. An independent
evidence audit checked counts/order/nonoverlap, hashes, all sixty rebuild
records, public-header identity, effective policies/affinity, regenerated
fingerprints, and the estimators without a significant evidence issue.
Three descriptive rounds are not perfect positional balance, equivalence
testing, or an assessment of other machines and storage devices.

Adoption is a bounded correctness/implementation-quality alignment and
measurement-contract repair, not numerical performance admission or a
claim that every component must independently win. The larger typed I/O
result and real multi-writer shared completion allocation remain explicit
residual differences. No exception, error, or durability guarantee was
disabled to manufacture a result.

## Sources

- [Pinned Native write pipeline](https://github.com/google/leveldb/blob/7ee830d02b623e8ffe0b95d59a74db1e58da04c5/db/db_impl.cc)
- [Pinned Native batch implementation](https://github.com/google/leveldb/blob/7ee830d02b623e8ffe0b95d59a74db1e58da04c5/db/write_batch.cc)
- [Pinned Native WAL writer](https://github.com/google/leveldb/blob/7ee830d02b623e8ffe0b95d59a74db1e58da04c5/db/log_writer.cc)
- [Pinned Native table builder](https://github.com/google/leveldb/blob/7ee830d02b623e8ffe0b95d59a74db1e58da04c5/table/table_builder.cc)
- [Pinned RocksDB coding helpers](https://github.com/facebook/rocksdb/blob/553b52b4a709820fac1dbadc7796506740517582/util/coding.h)
- [Pinned RocksDB block producer](https://github.com/facebook/rocksdb/blob/553b52b4a709820fac1dbadc7796506740517582/table/block_based/block_builder.cc)
- [Behavioral write parity](0060-leveldb-write-path-parity.md)
- [Compiler-visible read ownership and codecs](0091-native-get-cost-model-parity.md)
