# Table Cache, Snapshot and Seek-Support Lifecycle

## Scope and delivery

Design-first completion batch for engine/table_cache.cc/.h,
snapshot_registry.cc/.h and seek_statistics.cc/.h.
Source baseline: `cbf10ad3ba99a0bcd65002187be07127eeede008`.
Database read/lookup/version/table/cache implementations remain real caller
context and are unchanged.

This batch extracts one TableCache load transaction, intrusive snapshot
link/unlink stages and seek-record eligibility, then gives every current
definition and sampling lambda a bounded semantic disposition. Keep reviewed
design, original-source characterization and three production topics as local
commits, then publish one non-draft PR.

No public API, cache key, table open/read order, snapshot ordering, metadata seek
budget, sampling sequence, compaction scheduling, owner lifetime, mutex policy,
benchmark, runtime assert, diagnostic conditional, coverage exclusion or
threshold change.

## TableCache load transaction

Add private OpenTable(file_number, file_size), composing TableFileName,
OpenRandomAccess and Table::Open. It moves each successful owner to the next
stage, moves failures unchanged, and returns one unique Table only after full
open/validation. It does not consult or mutate the cache.

Find retains key construction and cache lookup first. On miss it calls OpenTable
then inserts the owner with charge one. Errors are never cached. A concurrent
Find may still open/insert a replacement; external handles retain either table.
Cached hits continue to ignore the supplied size.

CacheKey and Evict remain exact representation/pass-through leaves.

## Snapshot intrusive ownership

Add LinkNewest(registration), wiring the allocated node after the current tail
and before sentinel in the original pointer-write order. Add
Unlink(registration), wiring neighbors together without deleting or changing the
caller handle.

Register retains raw `new Registration(sequence)` before publication, calls
LinkNewest, increments size and returns a nonowning caller handle to the
registry-owned node. Registration constructor is noexcept, so raw allocation is
the only throwing stage.

Release calls Unlink, saves the node, nulls the caller handle, deletes the node,
then decrements size. Preserve the null-before-delete-visible return contract and
the existing private trivial-destructor coverage marker. No ownership wrapper,
runtime validation, sorting or thread synchronization.

Sentinel construction, oldest/OldestOr and sequence/size access remain unchanged.
The caller still guarantees nondecreasing registration sequences and a live
registration from this registry.

## Seek charge eligibility

Add ShouldRecord(version, current, file), called only after decrementing the
exact metadata object's allowed_seeks. It returns true only when the budget is
at most zero, version is current, and no file is already recorded for current.
Keep the three conditions in ordered statements rather than one compound
coverage-sensitive expression.

Charge retains file-slot borrowing and budget decrement, calls ShouldRecord,
then publishes the existing nonowning Recorded aggregate and returns true.
Negative budgets continue falling; old versions and already-recorded current
versions do not replace the record.

HasFileToCompact, FileToCompact, Retain and IsRecordedFor remain existing
query/owner materialization/lifetime operations. ReadSamplingPeriods and its
mutable Park-Miller lambda remain byte-for-byte deterministic.

## Selected eight-definition ledger

| Definition | Composition or stopping boundary |
| --- | --- |
| TableCache::OpenTable | New file acquisition and complete Table open owner transaction |
| TableCache::Find | Cache lookup, load-on-miss and charge-one insertion parent |
| SnapshotRegistry::LinkNewest | New intrusive tail insertion leaf |
| SnapshotRegistry::Unlink | New intrusive neighbor unlink leaf |
| SnapshotRegistry::Register | Allocate, link, count and return owner handle |
| SnapshotRegistry::Release | Unlink, clear caller handle, delete and count transition |
| SeekStatistics::ShouldRecord | New ordered exhausted/current/unrecorded eligibility |
| SeekStatistics::Charge | Decrement exact metadata and optional nonowning record publication |

## Thirty inspected unchanged definitions

### TableCache: nine

| Definition | Responsibility retained |
| --- | --- |
| CacheKey | Fixed64 file-number cache key |
| TableCache constructor | Move directory, copy options, borrow filesystem/comparator/block-cache pointee, construct owned typed cache |
| deleted comparator-rvalue constructor | Reject temporary comparator |
| deleted cache copy constructor | Preserve borrowed dependencies/cache owner |
| deleted cache copy assignment | Preserve borrowed dependencies/cache owner |
| deleted cache move constructor | Keep handles/dependencies stable |
| deleted cache move assignment | Keep handles/dependencies stable |
| defaulted cache destructor | Release cached tables after all handles |
| TableCache::Evict | Encode number and remove lookup reachability |

### SnapshotRegistry: sixteen

| Definition | Responsibility retained |
| --- | --- |
| Registration deleted copy constructor | Preserve unique intrusive node |
| Registration deleted copy assignment | Preserve unique intrusive node |
| Registration deleted move constructor | Preserve node address |
| Registration deleted move assignment | Preserve node address |
| Registration::sequence | Read immutable sequence |
| private Registration constructor | Initialize sequence/unlinked pointers |
| private Registration destructor | Trivial node destruction |
| SnapshotRegistry constructor | Initialize circular sentinel |
| deleted registry copy constructor | Preserve unique intrusive owner |
| deleted registry copy assignment | Preserve unique intrusive owner |
| deleted registry move constructor | Preserve node/sentinel addresses |
| deleted registry move assignment | Preserve node/sentinel addresses |
| defaulted registry destructor | Require all registrations released, destroy sentinel |
| SnapshotRegistry::size | Read caller-locked registration count |
| SnapshotRegistry::oldest | Empty or sentinel-next sequence |
| SnapshotRegistry::OldestOr | Empty fallback or sentinel-next sequence |

### SeekStatistics: five

| Definition | Responsibility retained |
| --- | --- |
| ReadSamplingPeriods | Construct deterministic mutable period generator |
| SeekStatistics::HasFileToCompact | Query record for current version |
| SeekStatistics::FileToCompact | Materialize owning file/level compaction |
| SeekStatistics::Retain | Clear record belonging to another version |
| SeekStatistics::IsRecordedFor | Nonowning version identity predicate |

The eight selected/new definitions plus these 30 rows yield 38 final named
definitions. The ReadSamplingPeriods closure is the one lambda body.

## Independent design review

Independent source-backed review corrected two ownership descriptions before
implementation. Snapshot registration pointers are nonowning handles to nodes
created/deleted solely by the registry. TableCache owns its moved directory,
copied TableOptions and typed cache while borrowing the filesystem, comparator
and copied block-cache pointee. No tests, builds or production edits were
performed during this bounded review.

## Original-source characterization

Existing tests cover cached reuse/eviction/zero capacity/failures/concurrency,
snapshot duplicates/oldest/middle/newest release/handle clearing, deterministic
sampling, exact seek budgets/current-version rules/retention/nonownership, and
database read/charge behavior.

Before production edits, add one TableCache case: retain a handle to file 7,
Evict it, rewrite the same numbered table with different contents, Find again,
and prove the old handle reads old bytes while the reopened handle reads new
bytes and the open count advances exactly once.

Use public subsystem operations, not private helpers. Do not infer single-flight
opens, allocator failure or unsupported registry misuse.

Freshly build modern_leveldb_unit_tests and the Windows native child on
unchanged production. Freeze exact unique identities for TableCacheTest,
SnapshotRegistryTest, ReadSamplingPeriodsTest, SeekStatisticsTest, LookupTest
and DatabaseTest, requiring nonzero counts.

Independently review characterization before production edits. Commit TableCache,
SnapshotRegistry and SeekStatistics topics separately with exact original
subsets. Finish with full identity parity and complete GCOVR/LCOV/NOLINT
annotation multiset equality for all six production paths.

Local GCC Release warnings-as-errors should compile changed/coupled objects with
`-fno-range-for-ext-temps`. Independent integrated review must reconcile all 38
definitions/one lambda, allocation/publication order, nonowning pointers,
metadata budgets and caller mutex assumptions. One published exact head must
pass every hosted check, authoritative GCC 13 branch coverage and actual Windows
Debug original-name parity before normal squash delivery.

Successful tests do not prove single-flight opens, arbitrary allocation failure,
invalid registry handles, concurrent registry/statistics access, scheduler
fairness, performance, or whole-production completion.
