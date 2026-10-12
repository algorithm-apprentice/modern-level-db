# Engine Final Completion

## Scope

Second ordered mainline task after native-platform completion. Source baseline:
`5fce3087fa553d304a0241f2e028b24d2c21368e`.

A fresh syntax scan covers all 32 files in `src/engine` and reports 315
`function_definition` nodes and 24 lambdas. Manual review adds nine deleted
free-function overload definitions that tree-sitter classifies only as recovery
nodes:

- two in `compaction.h`;
- four in `iterators.h`;
- two in `lookup.h`;
- one in `recovery.h`.

The actual original inventory is therefore 324 C++ definitions and 24 lambdas.
The remaining six recovery nodes are parser gaps around default arguments/type
identifiers, not additional definitions.

The complete mechanical index is retained as `engine-final-index.json` in
session evidence.

## Delivered engine groups

The final ledger preserves these accepted completions:

- complete compaction selection/execution lifecycle from #264;
- complete write queue and buffer-generation lifecycle from #257;
- complete TableCache, SnapshotRegistry and SeekStatistics support from #263;
- recovery input/replay-list/install composition;
- lookup source ordering, memory decisions and shared owned copying;
- merge-direction and user-iterator visibility transitions;
- trivial compaction and WAL rotation preparation;
- completed-table verification/metadata and table-writer tail composition.

Counts from those batches overlap and are not additive. Every current
definition and lambda is reviewed once again at the final source revision.

## Remaining frontier one: lookup visitor state

`LookupValue` still embeds table-search charging, previous-candidate tracking,
file lookup and final decision publication inside a large callback. Extract one
file-local `PointLookup` state owner containing only borrowed call dependencies
and owned traversal state:

- previous candidate;
- optional seek-charge publication;
- decided/value result state;
- `Visit(candidate)` for charge-before-second-file, `SearchFile`, Missing
  continuation and value/deletion stop;
- `result()` for the final found/deleted/missing boolean.

`LookupValue` keeps seek reset, memory-before-table priority, candidate
traversal, visitor-error propagation and final result composition. The
ForEachOverlapping callback becomes a one-expression forwarder.

`SampleCharge` similarly extracts a small `SampledOverlap` state owner:

- retain the first original candidate;
- count overlaps;
- stop after the second;
- materialize the first charge only when at least two exist.

The sampling callback also becomes a one-expression forwarder. Do not share the
two state types: point lookup performs I/O/error propagation, while sampling is
infallible metadata traversal.

Preserve lazy deeper-level selection, exact original-slot pointers,
charge-before-second-file-error timing, output sentinel behavior and all
comparator short-circuiting.

## Remaining frontier two: level seek positioning

`LevelIterator::Seek` currently composes candidate-file partitioning, table
opening, in-table seek, empty-table detection and next-file fallback.

Keep candidate selection and `Open(index)` in the public method. Extract private
`SeekInOpenFile(target, index)`:

1. seek the opened table;
2. propagate through existing `Fail`;
3. return when positioned;
4. otherwise seek the same table to first;
5. reject a physically empty table;
6. enter the following file or finish.

The helper assumes `Open(index)` succeeded and owns no handle. Preserve the
original error priority, same-table fallback and lazy opening of the next file.
Do not replace the partition search, call `Enter` recursively or add a generic
position callback.

Bulk merge seeks, winner scans and source assembly remain bounded compositions
or algorithmic leaves: their current loops are the operation rather than
unstructured orchestration.

## Remaining frontier three: compaction rewrite orchestration

`DatabaseEngine::Compact` still combines trivial-move selection with rewrite
setup, callback creation, unlocked execution, edit installation and completion.

Extract private `CompactionHooksFor(outputs)`:

- new-file callback allocates under `mutex_`, protects the output number and
  appends it to the caller-owned output vector;
- before-entry callback delegates to `BeforeCompactionEntry`.

Extract private `RewriteCompaction(compaction, lock, outputs)`:

1. capture the oldest visible snapshot;
2. build the compaction iterator;
3. prepare table/target options and callbacks;
4. unlock for `RunCompaction`;
5. relock;
6. apply the completed edit with the existing closing policy;
7. finish sticky-error/seek-budget handling.

`Compact` then owns only current-version pinning, compaction selection and the
trivial-versus-rewrite decision. It still returns false for a trivial move and
true for every rewrite attempt.

Callback construction and iterator/options allocation remain before unlock.
Any throw before unlock propagates with the lock held. `RunCompaction` exceptions
still escape while unlocked to `BackgroundCall`, whose existing catch relocks
before recording Aborted. Do not add catches, output cleanup, status
normalization or a generic background-operation framework.

## Complete stopping boundaries

After those three frontiers:

- recovery methods remain named transactions whose stream/batch/memtable state
  is deliberately coupled; splitting their inner loops would expose borrowed
  WAL record lifetimes or duplicate sequence/flush policy;
- BuildTable, WriteTable and FlushMemTable remain named build/verify/durability
  transactions over already separated children;
- Database read/write methods remain lock-ownership transactions with explicit
  unlock/relock and publication boundaries;
- DbIterator and merge-direction transitions retain their completed state
  machines;
- compaction, write path, read support and their private headers retain their
  complete delivered ledgers;
- constructors, special members, accessors, adapters and exhaustive predicates
  are reviewed leaves.

Do not introduce generic callback/state-machine frameworks, runtime assertions,
diagnostic conditional compilation or performance changes.

## Characterization and validation

Before production edits, freeze exact identities for:

- LookupTest and SampleChargeTest;
- LevelIteratorTest plus directly coupled MergingIteratorTest,
  InternalIteratorTest and MemTableIteratorTest;
- DatabaseTest/DatabaseSeekTest and CompactionTest;
- RecoveryTest, BuildTableTest, PickLevelTest and FlushMemTableTest as unchanged
  final-ledger context.

Existing tests already prove exact lookup order/charge pointers, early
value/deletion/error stop, lazy sampling, every iterator move/open failure,
empty-table corruption, compaction close/flush races, output tracking,
background exceptions and shutdown behavior. Add characterization only if
implementation review finds an unmeasured boundary.

Record exact unique JUnit before/after parity. Validate each topic separately,
then run the combined engine selection, Windows Debug/Release, complete Windows
Debug and authoritative Hosted GCC 13 coverage.

## Final ledger requirement

After implementation, rescan all engine files and manually reconcile parser
recovery nodes/deleted overloads. Every original and new definition/lambda must
receive:

- reviewed composition children, or
- a responsibility-based leaf stopping reason.

The final ledger is stored in session evidence and summarized here before
publication. Engine completion does not begin API or metadata/diagnostics work.

## Review focus

Review visitor-owned state lifetimes, original metadata addresses, Error move
boundaries, LevelIterator table/iterator ownership, callback capture lifetimes,
mutex ownership at every throw, output-number protection and exact
trivial-versus-rewrite return semantics.
