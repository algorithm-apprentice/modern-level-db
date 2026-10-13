# 08. C++ Ownership, Errors, and Concurrency

[Learning path](README.md) | Next: [Verification and performance](09-verification-and-performance.md)

Read this alongside any source tour where the C++ mechanisms are unfamiliar.

> **Advanced engineering lesson:** the first pass requires the ownership and
> synchronization conclusions, not mastery of every C++ memory-model detail.

## Lesson map

| Question | Answer |
|---|---|
| What problem is this lesson about? | Making lifetime, failure, publication, lock release, and shutdown arguments explicit |
| Who consumes these contracts? | Contributors implementing every engine layer |
| What interface matters first? | Ownership type, borrow lifetime, `Status`/exceptions, lock preconditions, and publication guarantees |
| What implementation will we open? | RAII handles, expected-based errors, acquire/release links, mutex/condition-variable protocols, and background teardown |
| What should you be able to do afterward? | Audit one pointer or unlocked operation and explain why its lifetime and synchronization are valid |

## Start with the lifetime question

For each pointer or view, ask:

```text
Who owns the object?
Who borrows it?
What keeps it alive while the mutex is unlocked?
What event invalidates the borrow?
```

These questions explain more of the design than "smart pointers are modern."

| Mechanism | Use here | Obligation |
|---|---|---|
| Value | Encoded metadata, options, stack writers | Define valid copy/move behavior |
| `unique_ptr` | Files, engines, iterator implementations | Exactly one owner controls destruction |
| `shared_ptr` | Engine state retained by public children, source ownership | Shared lifetime must have a concrete reason |
| Borrowed pointer/reference | Injected filesystem/comparator, active queue batch | Owner must outlive every borrower |
| `span<const byte>` | Input bytes and iterator values | Storage must remain alive and unchanged |
| RAII pin | Cache entries and read sources | Releasing the handle ends a documented retention |

`shared_ptr` protects lifetime, not the thread safety of the object it points
to. A non-null pointer is not a complete lifetime argument.

## RAII and move-only handles

**Resource acquisition is initialization**, or RAII, puts cleanup in an
owner's destructor so normal return and exception unwinding both release
resources.

Database, Snapshot, and Iterator are move-only public handles.
Moving transfers ownership; it does not duplicate the engine.
Their moved-from behavior is explicit rather than guessed by callers.
Some internal objects are non-movable because other members hold references
to their stable addresses.

The public facade hides implementations behind private state/PImpl types.
Consumers include public headers, not internal engine or codec headers.
This maintains a dependency boundary; it is not a promise of upstream ABI
compatibility.

### Destruction order is part of the design

C++ destroys members in reverse declaration order.
`Iterator::Impl` declares:

```text
engine state
snapshot registration
private DB iterator
```

Destruction runs in the opposite order:

```text
private iterator -> snapshot registration -> engine state
```

The iterator releases source/cache references while the engine still exists.
The snapshot releases its sequence before its engine lifetime ends.
Likewise, `DatabaseState` destroys the engine before its retained comparator.

This is why a child can safely outlive the public Database handle.
It also means an unreleased child can keep the directory lock held.

## Explicit error types

`Result<T>` is an alias for `std::expected<T, Error>`.
`Status` is `Result<void>`.
The signature says whether an operation can fail and whether success has a
payload.

```cpp
modern_leveldb::Status Store(modern_leveldb::Database& database) {
    auto written = database.Put(modern_leveldb::AsBytes("key"), modern_leveldb::AsBytes("value"));
    if (!written.has_value()) {
        return written;
    }
    return {};
}
```

`std::optional` means a value may be absent in an otherwise successful
operation. It does not replace an error:
`Get` uses both expected and optional because "key absent" is different from
"read failed."

Errors carry a code, message, and source location.
`[[nodiscard]]` makes accidental ignored results visible to the compiler.
It does not force a caller to handle them correctly.

Normal storage failures do not use exceptions for control flow.
That is not a guarantee that allocation cannot throw.
The commit and background paths have guards/containment for exceptions
that could otherwise strand queued writers or leave uncertainty unrecorded.
`noexcept` must match the actual operations, not a stylistic preference.

## Initialization before publication

The skip list provides a concrete acquire/release example:

```text
writer:
    construct entry and node
    initialize link
    predecessor.link.store(node, release)

reader:
    node = predecessor.link.load(acquire)
    inspect the published initialized node
```

When the acquire observes that release publication, earlier initialization
is visible to the reader. The stable arena lifetime then keeps the object
available.

Relaxed operations are used where a stronger ordering is unnecessary under
the actual invariant, such as writer-private link access or a height hint.
Do not copy `memory_order_relaxed` into another shared-state design without
reconstructing its visibility argument.

This component allows lock-free traversal of published links.
It does not make database writes, cache lookups, or the complete engine
lock-free.

## Mutexes and condition variables

The database mutex protects queue state, topology, snapshot registration,
sequence publication, background state, and related accounting.
Cache shards have their own locks.
Single-writer memtable insertion and immutable table reads follow different
contracts.

A `unique_lock` can temporarily release a mutex around slow I/O.
Before unlocking, the caller must have retained everything it will use.
On every return or exception path, it must restore the required lock state.

A condition-variable notification is not a stored "permission token."
Waiters recheck shared state:

```text
while the required condition is not true:
    wait, releasing the mutex
    reacquire and check again
```

The queue waits for "completed or now the leader."
Rotation waits for immutable/L0 progress, while also rechecking errors and
room conditions.
Spurious wakeups or another thread consuming progress must not break those
loops.

### Unknown destruction belongs outside a mutex

Removing an owning pointer or intrusive reference can execute a destructor.
When the value type belongs to a caller, that destructor is effectively a
callback: it can acquire locks, call another component, or reenter the
container that released it.

Commit the protected state transition first:

```text
under mutex: detach object, update indexes/accounting, record it for retirement
after unlock: run its destructor or deleter
```

The sharded LRU cache is the concrete example. It makes a zero-reference entry
unreachable under its shard mutex and destroys the retired value afterward.
See [lesson 05](05-tables-filters-and-caches.md#retire-under-the-mutex-reclaim-after-it).
The same question applies whenever a lock-protected container releases
user-visible ownership: identify whether that release can invoke unknown code.

## Background ownership and shutdown

The serial executor owns a `std::jthread` and uses stop-aware waiting.
Tasks are queued, not run inline by `Schedule`.
Inline execution would violate engine assumptions about locks and
background state.

The database destructor marks closing, prevents new background work, closes
its WAL, and waits for scheduled engine work to finish.
Accepted engine tasks must run while the engine is open; the owner cannot
destroy an injected executor too early.
Executor teardown stops and joins its worker before callback-visible state
is destroyed.

Public handles must not be moved or destroyed concurrently with operations
on those same objects. "Database methods support concurrent calls" does not
authorize a race with destruction.

Destructors cannot return a typed close failure. RAII guarantees resource
cleanup, not that unsynced updates became durable; request the required
write synchronization before relying on that guarantee.

## Proven invariants, not repeated guesses

There are two kinds of validation:

| Boundary | Appropriate behavior |
|---|---|
| Public argument, persistent bytes, I/O result | Check and return an explicit error |
| Established private invariant | Document and rely on the proven invariant; keep applicable compile-time checks and behavioral test assertions |

For a trusted path, identify its caller, established shape, protected
lifetime, and invalidation events.
An unchecked helper is not justified merely because a check was expensive.
Conversely, repeatedly checking an impossible state in every comparison
does not automatically provide meaningful safety.

The registry in [ADR-0060](../adr/0060-leveldb-write-path-parity.md) is a
useful advanced exercise in making those arguments explicit.

## Source tour

| Entry point | Interface or symbol | Question to answer | Representative evidence |
|---|---|---|---|
| [`result.h`](../../include/modern_leveldb/base/result.h) | `Error`, `Result`, `Status` | Which failures are ordinary typed results, and what may still throw? | [`result_test.cc`](../../tests/unit/base/result_test.cc) |
| [`api_internal.h`](../../src/api/api_internal.h) | public handle implementations | Which owner keeps the engine and child state alive? | [`public_api_test.cc`](../../tests/unit/api/public_api_test.cc) |
| [`skiplist.h`](../../src/memory/skiplist.h) | node initialization and publication | Which writes happen before a reader's acquire load can observe the node? | [`skiplist_test.cc`](../../tests/unit/memory/skiplist_test.cc) |
| [`write_path.h`](../../src/engine/write_path.h) | stack writer and leader/sequence guards | How does exception unwinding restore queue and batch state? | [`write_path_test.cc`](../../tests/unit/engine/write_path_test.cc) |
| [`database_read.cc`](../../src/engine/database_read.cc) | lock release and typed read pins | What keeps read sources alive while slow I/O runs without the mutex? | [`database_read_test.cc`](../../tests/unit/engine/database_read_test.cc) |
| [`database_background.cc`](../../src/engine/database_background.cc) | sticky failure, unlocked maintenance, shutdown | How do background transitions regain the mutex and preserve failure ownership? | [`database_background_test.cc`](../../tests/unit/engine/database_background_test.cc) |
| [`serial_executor.cc`](../../src/platform/serial_executor.cc) | queue ownership and stop/join | Who owns accepted work during teardown? | [`serial_executor_test.cc`](../../tests/unit/platform/serial_executor_test.cc) |
| [`sharded_lru_cache_core.cc`](../../src/cache/sharded_lru_cache_core.cc) | retired-entry reclamation | Which mutation must finish under the shard mutex before a user value deleter may reenter the cache? | [`sharded_lru_cache_test.cc`](../../tests/unit/cache/sharded_lru_cache_test.cc) |

Read [ADR-0004](../adr/0004-errors-ownership-and-runtime.md),
[ADR-0038](../adr/0038-public-raii-api.md), and
[code style](../development/code-style.md) for the project's contracts.

## Self-check

1. Does wrapping a mutable object in `shared_ptr` eliminate its data races?
2. Why is releasing a cache pin before using its value unsafe?
3. Why is the private iterator declared after the engine state?
4. Can an arbitrary disk buffer be passed to `OpenTrusted`?
5. Why should an object set its terminal state after, rather than before, a
   cleanup operation that can throw?
6. Why is moving an owning value out of a mutex-protected container not
   automatically enough to make destruction safe?

<details>
<summary>Answers</summary>

1. No. Shared ownership and synchronized access are different properties.
2. Releasing the last relevant owner can allow reclamation.
3. Reverse destruction destroys the iterator first, while its engine is live.
4. No. That path requires the owned batch's private validity invariant;
   external bytes need checked decoding.
5. If cleanup throws after the state was committed, the owner can retain an
   object that claims to be closed although the surrounding transition never
   completed.
6. Destroying the moved value can still run external code. The protected
   indexes, references, and accounting must already describe a complete state,
   and the destructor itself must run after unlocking.

</details>
