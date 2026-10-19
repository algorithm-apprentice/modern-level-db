# ADR-0078: Uniform Engine Dependency Ownership

- Status: Accepted
- Date: 2026-10-19

## Context

`DatabaseEngine` accepts borrowed filesystem, block-cache, clock, and executor
objects through its internal options. When an option is absent, it creates and
owns a default implementation. These independent choices are useful to unit,
fault, crash, and fuzz callers. Public injection is a new requirement covered
by [ADR-0079](0079-public-runtime-dependency-injection.md); it must use the same
engine assembly path rather than a separate implementation.

The current representation separates default owners from effective dependency
pointers. Filesystem, clock, and executor each have an `owned_*` smart pointer
and a raw pointer selecting the supplied or default object. The block-cache
owner similarly participates in selecting the pointer passed to `TableCache`.

One pointer and an ownership flag can express each dependency instead. The
important problem is preserving its lifetime, not preserving two pointer
members. A literal replacement with raw pointers and `delete` statements in
the engine destructor is insufficient:

- A failed ordinary constructor does not execute the complete object's
  destructor, although it destroys fully constructed subobjects.
- The engine destructor body runs before member destruction.
  `MemoryFileLock` destruction accesses its filesystem, and the table cache,
  versions, and WAL handles must also retain their dependency lifetimes.
- The owned executor must stop and join before callback-visible state is
  destroyed, with no database mutex held during its destruction.

[ADR-0004](0004-errors-ownership-and-runtime.md) requires RAII ownership.
[ADR-0009](0009-platform-runtime-services.md) defines scoped executor shutdown.
[ADR-0037](0037-database-engine.md) defines borrowed injection, and
[ADR-0074](0074-database-engine-coordination-structure.md) records the engine's
member-order lifetime constraints.

## Decision

Represent all four services with the same internal `Dependency<T>` handle:
a standard `std::unique_ptr<T, DependencyDeleter<T>>`, whose deleter contains
one `bool owns` and deletes the pointer only when that flag is true.

Define the small standard-library-only helper in `src/engine/dependency.h`:

- `DependencyDeleter<T>` supplies conditional deletion; its default flag is
  false.
- `Dependency<T>` names the smart-pointer specialization.
- `BorrowDependency(T&)` constructs a non-owning handle.
- `OwnDependency(std::unique_ptr<T>)` consumes an existing exclusive owner and
  constructs an owning handle.

Both construction helpers are `noexcept`. They perform no allocation;
`OwnDependency` releases its input only into a non-throwing smart-pointer
construction. Moving a handle moves its pointer and ownership policy together.
Do not manually modify the deleter's flag after construction.

This engine handle is not a public shared owner or general runtime framework.
It does not introduce an `Env`, an executor shutdown method, or another
allocation for storing a dependency. The public facade retains supplied
owners under ADR-0079 while the engine borrows their objects.

### Selection and construction

Keep lazy selection at the existing engine assembly boundary:

1. A supplied pointer becomes a borrowed handle without constructing a default.
2. An absent option creates the same default implementation as today and moves
   its owner into an owning handle.
3. Filesystem admission remains fallible and precedes engine construction.
   Return a `Result<Dependency<FileSystem>>` from that preparation stage.
4. Pass the prepared filesystem handle into the engine constructor. Construct
   the block-cache, clock, and executor handles in their current member order.
5. Continue to recover only after the engine is completely constructed.

Unlike a raw-pointer-only design, ownership need not wait until the complete
engine constructor succeeds. Each constructed dependency handle already owns
its object or explicitly borrows it. If a later member initialization throws,
normal member unwinding performs conditional cleanup in the correct order.
Temporary owners also protect allocation or argument-construction failures
before a dependency member exists.

There is no ownership-publication phase, mutable ownership bookkeeping, or
exception handler that attempts to read already-destroyed members.

### Declaration and destruction order

Use the same handle type for every service, but preserve different lifetime
positions:

```cpp
Dependency<FileSystem> file_system_;
Dependency<BlockCache> block_cache_;
Dependency<Clock> clock_;
std::unique_ptr<FileLock> lock_;
// Policy, paths, comparator, caches, synchronization, and mutable engine state.
Dependency<BackgroundExecutor> executor_;
```

The complete declaration retains the existing relative order of other members.
Filesystem/cache/clock handles precede every dependent member; the executor is
the last member.

Shutdown remains:

1. Execute the existing destructor body: mark closing, close the WAL, and wait
   for accepted background work.
2. Destroy its local `unique_lock`, releasing the database mutex.
3. Destroy the executor handle first. Delete and join an owned executor; leave
   an injected executor alive.
4. Destroy the engine's remaining state, cached tables, and directory lock.
5. Conditionally delete clock, block cache, and filesystem, in reverse order.

No service-specific manual `delete` is added to `DatabaseEngine`. Its existing
WAL-close exception handling and background-work protocol do not change.

### Preserved contracts

- Each option independently chooses owned default or borrowed injection.
  All 16 ownership combinations remain representable.
- Injected objects must outlive the engine, including its destructor. Tests can
  still operate an injected executor during shutdown and inspect filesystem
  state after closing or reopening.
- `DatabaseEngineOptions` remains copyable; callers do not transfer ownership
  by supplying a pointer.
- Default implementations, cache capacity, admission policy, lazy allocation,
  scheduling behavior, and error propagation remain unchanged. The public API
  additions are specified separately in ADR-0079.
- The comparator, file handles, memtables, versions, and public child-handle
  retention are outside this representation change.
- The engine remains non-copyable and non-movable.

## Alternatives

### Raw fields and manual conditional deletion

This matches the shape of LevelDB's optional cache ownership but would require
additional construction rollback and carefully staged destruction around our
value members. It does not simplify the lifetime proof.

### Transfer every injected object into an exclusive engine owner

This would change a useful existing borrowing contract and force fault/crash
callers to restructure their longer-lived state. It is unnecessary for removing
the duplicate pointers. Public facade ownership does not require the engine
to own injected objects.

### Separate executor cleanup mechanism

Executor shutdown needs a different lifetime position, not a different
ownership mechanism. The common handle and reverse member destruction already
express the required order.

### Custom owning-or-borrowing smart-pointer class

The standard smart pointer already supplies move-only ownership, dereferencing,
exception-safe construction, and stateful deletion. Reimplementing those
operations would add code without meeting another current requirement.

## Bounded implementation map

- `src/engine/dependency.h`: conditional deleter, alias, and two constructors.
- `src/engine/database.h`: one handle per service and preserved member order.
- `src/engine/database.cc`: lazy service selection, prepared filesystem handle,
  and direct use of the effective block-cache pointer.
- `tests/unit/engine/dependency_test.cc`: observable lifecycle tests.
- `tests/CMakeLists.txt`: register the new unit-test source.
- `docs/architecture.md`: update the current representation description.
- This ADR and the ADR index: record review and implementation outcome.

Other engine call sites already dereference the effective dependency pointers;
smart-pointer dereferencing preserves those expressions.
ADR-0079 owns the additional public contracts, facade wiring, and consumer tests.

## Validation and delivery

Review this proposal before editing production code. Use small commits for the
reviewed design, helper/tests, and engine integration.

Start with failing lifecycle tests, then implement the helper. Verify:

- owned objects are deleted exactly once, while borrowed objects survive;
- moves preserve both pointer identity and deletion policy;
- a later throwing member cleans earlier owning handles but not borrowed ones;
- mixed ownership choices preserve each resource's independent lifetime;
- executor cleanup precedes dependent state, and filesystem/cache/clock
  cleanup follows it, after destructor-body locals have been destroyed.

Then integrate the helper and run the existing engine lifecycle, native
filesystem, injected-cache, background shutdown, failure, and public API tests.
Existing lock-conflict and reopen tests must continue using their original
injected objects without test-storage redesign.

Run the documentation checker and the supported Windows Debug test preset.
The exact reviewed PR head must also pass the existing cross-platform,
sanitizer, compatibility, and 100% changed-code coverage gates before merge.
Do not weaken a gate or use exclusions to avoid testing either deletion branch.

Standalone lifetime experiments support the design, but are not evidence that
the integrated engine has passed regression tests. Record retained test and
CI evidence only after executing it. Make no object-size or performance claim
without a corresponding measurement.
