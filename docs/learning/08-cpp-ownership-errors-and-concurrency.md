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

Model owners and borrowers as a graph. An owning edge controls destruction;
a borrowing edge requires another path keeping the target alive.
Releasing one edge is safe only after every dependent borrow is finished
or another valid owner remains. Shared ownership cycles can keep objects
alive forever, so "never dangling" is not the complete resource goal.

| Mechanism | Use here | Obligation |
|---|---|---|
| Value | Encoded metadata, options, stack writers | Define valid copy/move behavior |
| `unique_ptr` | Files, engines, iterator implementations | Exactly one owner controls destruction |
| `shared_ptr` | Engine state retained by public children, supplied service owners, source ownership | Shared lifetime must have a concrete reason |
| Borrowed pointer/reference | Engine's injected services/comparator, active queue batch | Owner must outlive every borrower |
| `span<const byte>` | Input bytes and iterator values | Storage must remain alive and unchanged |
| RAII pin | Cache entries and read sources | Releasing the handle ends a documented retention |

`shared_ptr` protects lifetime, not the thread safety of the object it points
to. A non-null pointer is not a complete lifetime argument.

## A view is not a C++ reference

`ByteView` is the standard `span<const byte>` alias. It borrows bytes while
remaining a separate view object. Copying it copies the pointer/length
descriptor, not the key or value:

```cpp
const std::string storage = "abc";
const ByteView original = AsBytes(storage);
ByteView cursor = original;
cursor = cursor.subspan(1);
const ByteView& borrowed = original;
```

`original` and `borrowed` describe three bytes; the independent `cursor`
describes two. None owns `storage`. A `const ByteView&` borrows the descriptor
itself as well as referring indirectly to its backing bytes. Both must stay
stable for a synchronous read. A temporary view passed to a const-reference
parameter survives the call, but the callee must not retain a reference to
that descriptor. This does not extend the lifetime of its backing bytes.

This distinction explains why not every view should become a reference.
Returned views need their own descriptors: returning a reference to a local
descriptor would dangle even if its backing bytes still exist. A parser may
deliberately copy a cursor to avoid advancing its caller, or accept
`ByteView&` when it should advance the caller.

Selected non-inlined read boundaries instead use `const ByteView&`, matching
Native's `const Slice&` forwarding model. Whether a descriptor copy survives
depends on the calling convention and optimization context, not just the
fact that the view is nonowning. Inspect the actual caller; do not infer that
references are universally faster. Public override migration belongs in the
[API reference](../reference/api-and-options.md#comparator-and-bloom-requirements).

## RAII and move-only handles

**Resource acquisition is initialization**, or RAII, puts cleanup in an
owner's destructor so normal return and exception unwinding both release
resources.

Database, Snapshot, and Iterator are move-only public handles.
Moving transfers ownership; it does not duplicate the engine.
Their moved-from behavior is explicit rather than guessed by callers.
Some internal objects are non-movable because other members hold references
to their stable addresses.

For a move-only guard with a chosen empty-source contract, reason in states:

```text
move construction:
    source guards R -> destination guards R; source becomes empty
move assignment:
    release destination's previous D exactly once
    transfer R and empty source
self-move:
    follow the type's explicit policy, not an assumed universal C++ rule
```

The invariant assigns one release obligation to each guarded acquisition;
an emptied source must not release R again. This is an obligation at the
end of ownership, not a proof that every caller eventually drops its handle.
A shared pin follows a counted version of the invariant rather than unique
deletion. Not every C++ type
has this exact moved-from contract: the project's LookupKey, for example,
has a valid canonical representation after a move.

The public facade hides implementations behind private state/PImpl types.
Consumers include public headers, not internal engine or codec headers.
This maintains a dependency boundary; it is not a promise of upstream ABI
compatibility.

### Destruction order is part of the design

C++ initializes members in declaration order before executing the constructor
body. The initializer list does not change that order. Destruction first
executes the destructor body and destroys its locals, then destroys members
in reverse declaration order.
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
Likewise, `DatabaseState` destroys the engine before its retained `Options`.
Those options own any supplied comparator, filesystem, block cache, clock,
and executor. The engine borrows their objects, while the facade and its
children retain the owners.

This is why a child can safely outlive the public Database handle.
It also means an unreleased child can keep the directory lock held.

### One handle can own or borrow

An engine dependency uses an ordinary `Dependency<T>` member containing a
pointer and one fixed ownership flag. Its own destructor directly deletes a
default resource; an injected resource is left to its retaining owner.
Copying and assignment are forbidden. A move constructor transfers the pointer
and empties its source, without duplicating ownership. There is no callable
deleter: consumers explicitly access the resource through `get()`.

Every constructed owning member participates in exception unwinding even if
a later member fails and the engine's complete-object destructor never runs.
The same handle type works at different lifetime positions: filesystem,
cache, and clock precede their users, while executor is declared last and
is destroyed first. The destructor's local database lock has already been
released when executor deletion joins its worker.

This separates cleanup mechanism from cleanup order; it does not make a
borrowed object owned or extend its lifetime.

### Ownership and generated work are different proofs

RAII answers who releases a resource on normal return and exception unwinding.
It does not promise that every handle move, empty destructor, or accessor
disappears from optimized code. Several safe wrappers can still leave
outlined calls between layers.

The LRU lookup shows the separation:

```text
under shard mutex: find entry and acquire its reference
after unlock:      nonthrowing wrapping in the move-only pin
while pinned:      use the retained value
on release:        update references under the shard mutex
after unlock:      reclaim any retired value
```

No allocation, callback, or throwing step can intervene between acquiring
that reference and establishing its RAII guard. The pin retains an entry;
it still borrows the cache, whose owner must outlive every pin.

Tiny pin moves, null checks, and accessors are visible in private headers;
substantial lookup/reclamation work stays outlined. Header visibility lets
the compiler eliminate wrapper work, but `inline` alone is not proof that
it did. It also does not move synchronization obligations: database read pins
still require the database mutex during acquisition and release, including
after a lookup throws. Use behavioral tests for lifetime and
[generated-code/whole-workload evidence](09-verification-and-performance.md)
for cost. Neither replaces the other.

### Count cleanup separately from transfer

In a fixed-size guard model, move construction needs O(1) field work.
Move assignment also releases its destination's previous resource, so its
cost is O(1) plus that release cost. Final release can execute a deleter,
free a retained object graph, acquire locks, or perform native cleanup.
It is not universally constant-time just because the handle is small.

Count allocations, descriptor copies, reference transitions, and lock
acquisitions separately. Contention, callbacks, and I/O need their own
latency assumptions; a field-operation count cannot bound their wall time.
Inlining can remove empty bookkeeping, not required resource lifetime.

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

Storage reserved for an `expected` error alternative does not mean that an
Error is constructed or allocated on every success. Object size alone also
does not establish its return convention or runtime cost.

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

The argument composes three edges: initialization is sequenced before the
release store; the observing acquire synchronizes with that publication;
subsequent field inspection follows the acquire. Together they establish
the required happens-before relation. The proof additionally requires no
unsynchronized mutation of those immutable fields and no premature free.
Changing only the memory-order spelling does not establish those premises.

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

Trace one reader against a rotating writer:

```text
reader under mutex: retain old memtable/Version pins
reader unlocks:     lookup can use their immutable entries
writer:             rotate/flush/install, retaining pinned old owners
reader:             finish lookup or catch an exception, then relock
reader under mutex: release pins; final retention can end
```

Without the first pin, the writer could reclaim the source during lookup.
Without relocking on the exception path, pin counters and retained-owner
state could be changed outside their synchronization domain.

Conversely, holding the database mutex is not proof for every field:
mutable arena accounting can change during an unlocked write commit.
An observation needs a published value or the actual writer synchronization,
not a mutex that happens to protect neighboring metadata.

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
The engine-owned executor stops and joins its worker before callback-visible
state is destroyed. A supplied executor is not stopped by the engine; it must
continue making progress until the engine's accepted work has finished.

Public handles must not be moved or destroyed concurrently with operations
on those same objects. "Database methods support concurrent calls" does not
authorize a race with destruction.

Destructors cannot return a typed close failure. RAII guarantees resource
cleanup, not that unsynced updates became durable; request the required
write synchronization before relying on that guarantee.

### Batch lifetime and read consistency are separate proofs

A joined worker can safely borrow a result slot without guaranteeing that all
keys observe one database sequence. Conversely, retaining a sequence does not
keep a destroyed result vector alive. The bounded batch read needs both proofs:

```text
retain engine/services and one registered read sequence
    -> allocate fixed result slots
    -> run bounded readers on disjoint positions
    -> stop new assignments on exceptions and join all started workers
    -> publish complete results or rethrow
    -> release registration and engine owners
```

The sequence registration preserves logical history across gaps between Gets;
it does not pin an old SST or Version forever. A decisive test releases the
first Get's sources, overwrites/deletes and rewrites the old SST away, then
checks that a later batch key still sees the registered sequence.

Failed startup needs the same lifetime argument as success. A started reader
may be blocked in synchronous I/O when another thread cannot be created.
Cleanup must stop the next assignment, wait without holding the database mutex,
and retain borrowed buffers and services until the blocked read finishes.
An Io result remains a per-key error; an allocation exception is joined and
re-thrown, never published as a missing key.

See the [batch implementation](../../src/api/multi_get.cc) and
[actual failure schedules](../../tests/unit/api/multi_get_internal_test.cc).
More parallelism is not a latency guarantee: per-batch startup and owning
output have measured costs, especially for one-key batches or applications
that already keep persistent read workers.

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

## Exercises: prove ownership, publication, and cleanup

1. Does wrapping a mutable object in `shared_ptr` eliminate its data races?
2. Why is releasing a cache pin before using its value unsafe?
3. Why is the private iterator declared after the engine state?
4. Can an arbitrary disk buffer be passed to `OpenTrusted`?
5. Why should an object set its terminal state after, rather than before, a
   cleanup operation that can throw?
6. Why is moving an owning value out of a mutex-protected container not
   automatically enough to make destruction safe?
7. Does copying a ByteView copy its bytes? Does a reference to that view own
   or extend the backing storage?
8. If a destructor is defined in a header, can you conclude its cost has
   disappeared or its locking preconditions no longer matter?
9. **State:** trace move assignment when both guards own resources. At
   which transition is the old destination released, and what would cause
   a double release?
10. **Interleaving:** remove the initial reader pin from the rotation trace.
    Construct the exact use-after-reclamation sequence.
11. **Publication:** which premise is missing if a reader uses an acquire
    load but the writer mutates the ordinary payload after publication?
12. **Transfer:** design a guard around a different asynchronous resource.
    Draw its owner/borrow graph, terminal event, mutex obligations, and
    cleanup-error policy before choosing a smart-pointer type.

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
7. Neither. Copying copies the descriptor; a reference borrows that descriptor.
   Both still rely on a separate backing owner.
8. No. Check actual generated work separately. Ownership, lock state, and
   exception cleanup remain correctness obligations regardless of inlining.
9. Release the old destination before replacing its ownership. The source
   must become nonowning; otherwise both destructors can release R.
10. The reader borrows an entry, unlocks, the writer retires and destroys
    its arena, and the reader dereferences the old pointer.
11. Immutable or synchronized post-publication access is missing. Seeing
    initialized data does not authorize racing later writes.
12. A valid answer identifies every borrower and the event ending its use.
    Cleanup must respect that event even when an operation reports an error.

</details>

## Source tour

| Entry point | Interface or symbol | Question to answer | Representative evidence |
|---|---|---|---|
| [`bytes.h`](../../include/modern_leveldb/base/bytes.h), [`comparator.h`](../../include/modern_leveldb/base/comparator.h) | view objects and borrowed read descriptors | Which object is copied or borrowed, and which storage lifetime remains external? | [`bytes_test.cc`](../../tests/unit/base/bytes_test.cc), [`comparator_test.cc`](../../tests/unit/base/comparator_test.cc) |
| [`result.h`](../../include/modern_leveldb/base/result.h) | `Error`, `Result`, `Status` | Which failures are ordinary typed results, and what may still throw? | [`result_test.cc`](../../tests/unit/base/result_test.cc) |
| [`api_internal.h`](../../src/api/api_internal.h) | public handle implementations | Which owner keeps the engine and child state alive? | [`public_api_test.cc`](../../tests/unit/api/public_api_test.cc) |
| [`dependency.h`](../../src/engine/dependency.h) | conditional dependency ownership | How do one pointer and one flag preserve ownership through moves and partial construction? | [`dependency_test.cc`](../../tests/unit/engine/dependency_test.cc) |
| [`skiplist.h`](../../src/memory/skiplist.h) | node initialization and publication | Which writes happen before a reader's acquire load can observe the node? | [`skiplist_test.cc`](../../tests/unit/memory/skiplist_test.cc) |
| [`write_path.h`](../../src/engine/write_path.h) | stack writer and leader/sequence guards | How does exception unwinding restore queue and batch state? | [`write_path_test.cc`](../../tests/unit/engine/write_path_test.cc) |
| [`database_read.cc`](../../src/engine/database_read.cc) | lock release and typed read pins | What keeps read sources alive while slow I/O runs without the mutex? | [`database_read_test.cc`](../../tests/unit/engine/database_read_test.cc) |
| [`database_background.cc`](../../src/engine/database_background.cc) | sticky failure, unlocked maintenance, shutdown | How do background transitions regain the mutex and preserve failure ownership? | [`database_background_test.cc`](../../tests/unit/engine/database_background_test.cc) |
| [`serial_executor.cc`](../../src/platform/serial_executor.cc) | queue ownership and stop/join | Who owns accepted work during teardown? | [`serial_executor_test.cc`](../../tests/unit/platform/serial_executor_test.cc) |
| [`sharded_lru_cache_core.h`](../../src/cache/sharded_lru_cache_core.h), [`sharded_lru_cache_core.cc`](../../src/cache/sharded_lru_cache_core.cc) | inline pin envelope and outlined retention/reclamation | What makes pin wrapping nonthrowing, and which invariant must be restored before a value deleter reenters? | [`sharded_lru_cache_test.cc`](../../tests/unit/cache/sharded_lru_cache_test.cc) |

Read [ADR-0004](../adr/0004-errors-ownership-and-runtime.md),
[ADR-0038](../adr/0038-public-raii-api.md), and
[code style](../development/code-style.md) for the project's contracts.
