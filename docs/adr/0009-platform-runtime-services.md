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

## Decision

Add internal platform-layer clock and background-executor abstractions.

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
virtual Status Shutdown() = 0;
```

`SerialExecutor` owns one `std::jthread` and executes accepted tasks one at a
time in FIFO order.

- `Schedule` is thread-safe.
- An empty task returns `InvalidArgument`.
- Scheduling after the executor stops accepting work returns `Aborted`.
- A task accepted concurrently with shutdown may be canceled before it starts.
- `Shutdown` rejects calls from the worker thread with `InvalidArgument`.
- External `Shutdown` is thread-safe and idempotent.
- Shutdown stops accepting work, destroys queued tasks without executing them,
  requests stop for the running task, wakes the worker, and joins it.
- Canceled task objects are destroyed after executor locks are released, so
  callable cleanup may safely attempt another executor operation.
- One external caller owns shutdown. Other external callers wait until worker
  join and canceled-task destruction are complete. Reentrant shutdown on the
  owner thread, including from synchronous stop callbacks or callable cleanup,
  returns success without waiting on itself.
- Callable construction, copying, movement, and destruction occur outside the
  queue mutex. The queue stores only owning task pointers while locked.
- A running task receives the worker's stop token and must cooperate for prompt
  shutdown. Shutdown waits if it ignores the request.
- Tasks must not let exceptions escape. An uncaught task exception follows the
  C++ thread contract and terminates the process.
- The executor must not be destroyed by one of its own tasks. Its owner
  destroys it from an external thread; the destructor performs shutdown.

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
- Engine-specific background-error state or compaction scheduling policy.
- A fake clock or deterministic executor implementation.

Test implementations can derive from the interfaces when higher-layer nodes
need deterministic control.

## Consequences

- Engine code can depend on explicit clock and executor interfaces instead of
  a monolithic environment object.
- FIFO serialization matches the initial single-background-worker engine
  design and avoids premature parallel-compaction policy.
- Shutdown and cancellation have explicit ownership and observation points.
- Copyable tasks are a temporary portability constraint.

## Validation

Clock tests cover monotonic reads, non-positive durations, and pre-requested or
asynchronously requested stop without long sleeps.

Executor tests cover FIFO serialization, a stable worker thread, concurrent
scheduling, copyable callable ownership, empty-task and stopped-executor
errors, stop-token delivery, cancellation of queued work, self-shutdown
rejection, idempotent external shutdown, and destructor-driven shutdown.
Asynchronous tests use latches and bounded CTest timeouts instead of arbitrary
sleeps.
