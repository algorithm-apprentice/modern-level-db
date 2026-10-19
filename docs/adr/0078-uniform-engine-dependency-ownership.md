# ADR-0078: Uniform Engine Dependency Ownership

- Status: Accepted
- Date: 2026-10-19

This amendment replaces the first implementation's smart-pointer
specialization with an ordinary value member. Public injection and all
service-lifetime positions remain unchanged.

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

Represent all four services with the same ordinary `Dependency<T>` value
member. It stores one `T*` and one ownership flag. Its destructor directly
deletes the pointer only when the flag is true.

Define the small standard-library-only helper in `src/engine/dependency.h`:

- `Dependency<T>` owns or borrows one object and exposes only `get()`.
- Copy construction and all assignment are deleted. Selected services are
  not replaceable during the engine lifetime.
- A non-throwing move constructor transfers the pointer and copies the fixed
  ownership flag, leaving the source pointer null. The existing expected-based
  filesystem factory and engine construction require this transfer.
- `BorrowDependency(T&)` constructs a non-owning handle.
- `OwnDependency(std::unique_ptr<T>)` consumes an existing exclusive owner and
  constructs an owning handle.

Both construction helpers are `noexcept`. They perform no allocation;
`OwnDependency` releases its input only into non-throwing member construction.
The ownership flag does not change after construction. An empty owner and a
moved-from owner are valid: deleting a null pointer has no effect.

There is no custom deleter callback, function-call operator, pointer-access
operator, `reset`, `release`, or move assignment. Consumers explicitly access
the selected service through `get()`. Temporary factory owners still use
`unique_ptr` to guard allocations before the value member takes ownership.

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

### Standard unique pointer with a conditional deleter

The first implementation used `unique_ptr<T, DependencyDeleter<T>>`. It
correctly handled lifetime and moves, but exposed a callable deleter and a full
smart-pointer interface for services that are only selected, accessed, and
destroyed. The value member makes the pointer/flag/destructor relationship
direct, without recreating the unused reset/release/assignment operations.

## Bounded implementation map

- `src/engine/dependency.h`: the value member and two construction helpers.
- `src/engine/database{,_write,_background}.cc`: explicit service access.
- `tests/unit/engine/dependency_test.cc`: observable lifecycle tests.
- `docs/architecture.md`: update the current representation description.
- This ADR and the ADR index: record review and implementation outcome.

The four declarations in `database.h` retain their existing positions.
No public options, cache, filesystem, or executor interface changes are needed.
ADR-0079 owns the additional public contracts, facade wiring, and consumer tests.

## Validation and delivery

Review this proposal before editing production code. Use small commits for the
reviewed design, helper/tests, and engine integration.

Start with failing lifecycle tests, then implement the helper. Verify:

- owned objects are deleted exactly once, while borrowed objects survive;
- moves preserve both pointer identity and deletion policy;
- borrowed moves and empty/moved-from owners remain safe for each service kind;
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

## Earlier implementation

The first implementation used the same conditional-deleter handle for all four
services. Filesystem admission returned the prepared handle, and successful
member initialization immediately established its deletion policy. There were
no `owned_*` service members, post-construction flag mutations, or executor-only
manual deletion paths. The amendment preserves those lifetime rules.

Retained dependency tests exercised owned/borrowed cleanup, move construction
and assignment, and every four-service ownership mixture across partial
construction failures and normal shutdown. Existing engine and public
dependency tests retain their original service-lifetime contracts.
