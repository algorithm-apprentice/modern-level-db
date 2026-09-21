# ADR-0009: Platform Runtime Services

- Status: Accepted
- Date: 2026-09-21

## Context

The original LevelDB `Env` combines files, clocks, sleeping, detached threads,
and background scheduling. Modern LevelDB separates those concerns so engine
policy can be tested independently from operating-system I/O.

This implements only the `implement-platform-runtime` DAG node. Filesystem
services, compaction policy, write throttling, and engine shutdown remain
separate future nodes.

## Prior art and adopted decisions

This runtime design combines established behavior rather than introducing a
new executor model:

- **Google LevelDB `Env`:** adopt one background worker and FIFO execution.
  Replace its detached process-lifetime thread, raw function pointer, `void*`
  argument, and lack of shutdown with scoped ownership and typed callables.
- **C++20 `std::jthread` and stop tokens:** adopt RAII joining and cooperative
  cancellation. `request_stop()` invokes registered callbacks synchronously on
  the requesting thread, so stop requests must execute without executor locks
  held.
- **C++ Core Guidelines CP.22:** adopt the rule that callbacks and other
  unknown code must not run while holding a lock. Callable lifecycle and task
  invocation therefore occur outside executor mutexes.
- **Boost.Asio `thread_pool`:** adopt explicit completion through joining and
  the separation between task submission and worker ownership. Do not adopt a
  multi-thread pool because the initial engine requires serialized background
  work or a public join protocol because the DB exclusively owns the executor.

RAII destruction is the minimal adaptation needed to combine those established
rules with Modern LevelDB's single-owner lifecycle.

## Decision

Add internal platform-layer clock and background-executor abstractions.

## Intended engine use

`SerialExecutor` is not a general-purpose application executor. The future DB
engine will use it under these constraints:

- One DB instance exclusively owns one serial executor.
- Foreground read/write threads may call `Schedule` concurrently.
- Scheduled work consists of coarse internal maintenance callbacks such as
  memtable flush and compaction, not arbitrary user callbacks.
- Engine state prevents duplicate background scheduling and decides whether
  more work is needed after a callback completes.
- Before destroying the executor, the DB owner prevents new scheduling.
- Executor destruction happens before any state that a background callback may
  access is destroyed.
- A running callback observes the stop token and returns promptly; queued work
  may be canceled because the owning DB is closing.

This model does not require a public shutdown API, concurrent shutdown calls,
executor restart, or reuse after stopping. Those capabilities would add states
and synchronization that the current engine cannot use.

### Clock

`Clock` exposes:

```cpp
using TimePoint = std::chrono::steady_clock::time_point;
using Duration = std::chrono::steady_clock::duration;

virtual TimePoint Now() const noexcept = 0;
virtual bool SleepFor(Duration duration, std::stop_token stop_token = {}) = 0;
```

`SystemClock` implements the interface with `std::chrono::steady_clock`.
Wall-clock time is intentionally excluded: elapsed-time measurements and
throttling must not jump when civil time changes.

`SleepFor` has these semantics:

- A pre-requested or observed stop returns `false`.
- A non-positive duration returns immediately and returns `true` unless stop
  was already requested.
- A completed positive duration returns `true`.
- A stop request interrupts a positive sleep and returns `false`.

The method may throw platform synchronization failures; it does not convert
programmer or runtime-library failures into storage `Status`.

### Background executor

`BackgroundExecutor` exposes:

```cpp
using BackgroundTask = std::function<void(std::stop_token)>;

virtual Status Schedule(BackgroundTask task) = 0;
```

`SerialExecutor` owns one `std::jthread` and executes accepted tasks one at a
time in FIFO order.

- `Schedule` is thread-safe.
- An empty task returns `InvalidArgument`.
- The executor is exclusively owned. Its owner stops producers before
  destroying it; scheduling concurrently with destruction is outside the
  object-lifetime contract.
- Destruction stops accepting work, destroys queued tasks without executing
  them, requests stop for the running task, wakes the worker, and joins it.
- Canceled task objects are destroyed after executor locks are released, so
  callable cleanup does not run inside a critical section. A cleanup callback
  that attempts to schedule work observes that the executor is no longer
  accepting tasks and receives `Aborted`.
- Callable construction, copying, movement, and destruction occur outside the
  queue mutex. The queue stores only owning task pointers while locked.
- A running task receives the worker's stop token and must cooperate for prompt
  destruction. The destructor waits if it ignores the request.
- Tasks must not let exceptions escape. An uncaught task exception follows the
  C++ thread contract and terminates the process.
- The executor must not be destroyed by one of its own tasks. Its owner
  destroys it from an external thread.

#### RAII shutdown sequence

The owner destroys the executor after preventing new external scheduling:

1. Under the queue mutex, stop accepting tasks and detach the pending queue.
2. Without the queue mutex held, request stop, run synchronous stop callbacks,
   wake and join the worker, and destroy canceled tasks.
3. Destroy the remaining synchronization state after the worker has exited.

The queue mutex protects only the acceptance flag and owning task pointers.
Task construction, copy/move/destruction, invocation, and stop callbacks never
run while it is held.

`BackgroundTask` uses `std::function` because the current supported macOS
standard library does not yet provide C++23 `std::move_only_function`.
Therefore scheduled callables must be copy-constructible. This may be revisited
when all supported standard libraries provide the standard move-only wrapper;
this node does not introduce a custom type-erasure implementation.

The executor depends on `Threads::Threads` through its library interface so
embedded consumers receive the platform thread linkage required by the static
library.

## Non-goals

The initial runtime layer does not provide:

- A thread pool or parallel task execution.
- Priorities, delayed scheduling, periodic tasks, or work stealing.
- Detached fire-and-forget threads.
- Futures, task return values, or exception transport.
- A public shutdown protocol, concurrent shutdown calls, or executor reuse
  after stopping.
- Engine-specific background-error state or compaction scheduling policy.
- A fake clock or deterministic executor implementation.

Test implementations can derive from the interfaces when higher-layer nodes
need deterministic control.

## Consequences

- Engine code can depend on explicit clock and executor interfaces instead of
  a monolithic environment object.
- FIFO serialization matches the initial single-background-worker engine
  design and avoids premature parallel-compaction policy.
- RAII ownership makes shutdown a single-owner lifecycle operation instead of
  a general concurrent protocol.
- Copyable tasks are a temporary portability constraint.

## Validation

Clock tests cover monotonic reads, non-positive durations, and pre-requested or
asynchronously requested stop without long sleeps.

Executor tests cover FIFO serialization, a stable worker thread, concurrent
scheduling, copyable callable ownership, empty-task errors, callable lifecycle
outside locks, stop-token delivery, rejection of cleanup scheduling during
destruction, queued-task cancellation, and destructor-driven shutdown.
Asynchronous tests use latches and bounded CTest timeouts instead of arbitrary
sleeps.

## References

- [Google LevelDB POSIX background queue](https://github.com/google/leveldb/blob/7ee830d02b623e8ffe0b95d59a74db1e58da04c5/util/env_posix.cc#L808-L852)
- [C++ Core Guidelines CP.22: never call unknown code while holding a lock](https://isocpp.github.io/CppCoreGuidelines/CppCoreGuidelines#cp22-never-call-unknown-code-while-holding-a-lock-eg-a-callback)
- [`std::stop_source::request_stop` synchronous callback semantics](https://en.cppreference.com/w/cpp/thread/stop_source/request_stop)
- [`std::jthread`](https://en.cppreference.com/w/cpp/thread/jthread)
- [Boost.Asio `thread_pool`](https://www.boost.org/doc/libs/latest/doc/html/boost_asio/reference/thread_pool.html)
