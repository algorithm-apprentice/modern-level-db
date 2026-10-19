# ADR-0079: Public Runtime Dependency Injection

- Status: Implemented
- Date: 2026-10-19

## Context

The filesystem, block cache, clock, and background executor have been injectable
only through `DatabaseEngineOptions`, under private source headers. Users of
the supported `Database` API cannot provide them. Merely simplifying internal
ownership would not satisfy the requirement to accept user-supplied services.

The public RAII facade has an additional lifetime constraint: iterators and
snapshots retain the engine after the originating `Database` handle is gone.
Requiring an injected object to outlive only that handle would be incorrect.
The facade already retains a shared custom comparator for exactly this reason.

There are two separate contracts:

- The public facade retains supplied owners for the complete engine lifetime.
- The engine borrows supplied objects and conditionally owns defaults, using
  [ADR-0078](0078-uniform-engine-dependency-ownership.md).

This ADR amends the public-injection exclusion in
[ADR-0038](0038-public-raii-api.md). It does not merge the platform services into
an `Env` or require every injected service to have an exclusive owner.

## Decision

### Public options and retained lifetime

Append four default-empty fields to public `Options`, after all existing
aggregate members:

```cpp
std::shared_ptr<FileSystem> file_system{};
std::shared_ptr<BlockCache> block_cache{};
std::shared_ptr<Clock> clock{};
std::shared_ptr<BackgroundExecutor> executor{};
```

An empty field selects the existing default. A nonempty field selects that
exact object. These choices are independent, and `Options` remains copyable.
Appending fields with default member initializers preserves existing
positional aggregate initialization.

Shared ownership here has a concrete lifetime reason, not an assumption that
every service must be globally shared. Callers may construct an exclusive
object and move it into a shared owner before opening a database. They may
also intentionally share a suitable cache, filesystem, or executor across
database instances.

`Database::Open` passes raw `.get()` pointers into internal engine options.
Its by-value public options protect supplied objects throughout admission,
engine construction, and recovery. On success, move those options into
`detail::DatabaseState`, declared before its engine owner. That shared state
already survives through retained iterators and snapshots.

Consequently:

- Supplied services remain alive until the last engine-retaining handle is
  released, even if the caller releases its own copies.
- The engine is destroyed before retained options release their owners.
- In the appended-field order above, retained executor ownership is released
  before clock, cache, and filesystem ownership.
- Failed opens release temporary retained ownership without publishing a
  database; they do not delete objects still owned by the caller.
- Mutating or destroying the caller's `Options` after opening does not replace
  an engine's selected dependencies.

Document that destruction may wait for accepted executor work. Keeping an
executor alive is not the same as making progress: an implementation whose
event loop is blocked by database destruction cannot satisfy that contract.

### Filesystem

Expose the existing std-only filesystem and file-handle contracts in
`include/modern_leveldb/file_system.h`: sequential files, random-access files,
writable files, file locks, and `FileSystem`.

Name the directory operations `CreateDir` and `RemoveDir`. The former private
names collide with the Windows SDK's `CreateDirectory` and `RemoveDirectory`
macros. Public consumers must compile with either header order without
undefining the SDK's macros. Update internal adapters and callers to the same
virtual names; explicit native W-function calls are unchanged.

Keep `CreateDefaultFileSystem` publicly available with its existing explicit
path/admission arguments. This lets users delegate to an admitted native
backend rather than reimplement every filesystem operation. It does not grant
strong namespace durability to an injected Windows backend; an injected
implementation remains responsible for its own advertised policy.

Keep `src/platform/file_system.h` as an internal forwarding include to avoid
unrelated include churn. Native POSIX/Windows implementations remain private.
Do not expose platform-native handles or operations-test hooks.

### Clock

Expose the existing abstract `Clock` in
`include/modern_leveldb/clock.h`. Its time points use `steady_clock`;
`Now()` remains non-throwing and `SleepFor` retains its stop-token result.
Elapsed time must not depend on civil-clock changes.

Keep the concrete `SystemClock` and implementation under `src/platform`.
An empty option uses that implementation. Users implementing a clock must
provide the concurrency behavior required by their database instances.

### Background executor

Expose `BackgroundTask` and the existing abstract `BackgroundExecutor` in
`include/modern_leveldb/background_executor.h`. Keep `SerialExecutor` private.

The public scheduling contract must state:

- Scheduling is safe for concurrent producers.
- A successful call accepts the task exactly once for eventual execution.
- Execution is deferred, never inline within `Schedule`, which the engine can
  call while holding its mutex.
- A failed or throwing call does not retain or execute the task.
- Accepted engine work must make progress during engine shutdown.
- Task exceptions must not escape an execution thread.

The engine continues waiting for its own accepted work. It does not stop or
destroy an injected executor that other owners may still use.

### Block cache without private format types

The existing `BlockCache` alias exposes `ShardedLruCache<Block>`, including a
private decoded-block type and retention handles. Do not move those storage
implementation types into public headers just to add an option.

Expose an opaque `BlockCache` declaration in
`include/modern_leveldb/block_cache.h`, together with:

```cpp
std::shared_ptr<BlockCache> CreateBlockCache(std::size_t capacity);
std::size_t BlockCacheCharge(const BlockCache& cache);
```

Define the complete internal `BlockCache` in `src/table/block_cache.h` as a
small composition over the existing typed cache. Preserve its internal
`Handle`, `Insert`, `Lookup`, `NewId`, and `total_charge` operations so table
algorithms and their existing tests do not need a second cache mechanism.
Inline forwarding does not require another cache-storage allocation or
type-erased callback in the read path.

Implement the public factory and charge observation in the API layer. Shared
owners support an incomplete public type, so callers do not need its private
definition to retain, pass, or release a cache.

Capacity remains the existing eviction target, not a strict process-memory
limit. Charge excludes unrelated allocator/metadata overhead; retained pins
can delay eviction. Zero capacity is supported. Existing per-table namespace
IDs prevent collisions when one cache serves different databases.

This first contract supports user-selected cache capacity, retention, and
cross-database sharing. Arbitrary custom eviction-policy implementations and
exposing private block values are not required.

## Bounded implementation and review slices

1. Review ADR-0078 and this proposal before production changes.
2. Implement the internal conditional-ownership helper and focused tests.
3. Expose the std-only platform contracts and opaque cache factory; keep
   existing private implementations and compatible internal include paths.
4. Wire public options and retained shared state, with public-consumer tests.
5. Integrate uniform engine handles and reconcile current documentation.
6. Independently review the exact implementation, pass all gates, publish as
   the requested GitHub identity, and merge only the reviewed passing head.

Use separate topic-scoped commits. Do not fold in unrelated platform,
background-policy, cache-algorithm, or persistent-format changes.

## Required evidence

Public-consumer tests must build using supported include directories, without
private engine/platform/cache headers. Cover:

- both include orders with the Windows SDK, preserving its directory macros;
- independently empty/supplied option fields and legacy aggregate ordering;
- a supplied filesystem's explicit error reaching `Database::Open`;
- real injected executor scheduling, clock use, and block-cache charge;
- retaining supplied services after the caller and `Database` release them,
  until the last iterator or snapshot is released;
- cleanup after admission or recovery failure;
- reusing a filesystem across close/reopen;
- two databases sharing a cache without mixing keys or values;
- default behavior when no service is supplied.

Use deterministic scheduling and observable counters rather than sleeping or
checking private field names. Retain existing internal fault/crash tests,
which continue borrowing fixture-owned objects without API changes.

Update the public options/reference documentation, ownership learning
material, architecture, and relevant source entry points. Mark the historical
public-injection exclusion as amended rather than rewriting its original
rationale.

Local supported-platform tests and documentation checks complement, but do not
replace, the existing cross-platform and sanitizer CI gates. No performance,
strict-memory-bound, arbitrary-plugin, or new durability guarantee follows
merely from exposing these inputs.

## Implementation outcome

Public options now accept all four supplied owners. `DatabaseState` retains
the complete options before its engine member, preserving the same shared
lifetime through both iterator and snapshot handles. Platform contract
headers and the opaque block-cache factory compile in the public consumer
target without private include directories.

Retained tests verify supplied filesystem error/exception propagation,
cleanup after failed opens, real executor/clock/cache use across reopen,
retention until the last child, and cache sharing between distinct databases.
No internal fault/crash caller was converted from borrowing to exclusive
ownership.
