# Native Support Lifecycle Completion

## Scope

Hardest-first completion for the native scheduling and timing support layer.
Source baseline: `63d640992ec469185bba10174a2062ec2b7501ce`.

This batch covers:

- `src/platform/background_executor.h`
- `src/platform/serial_executor.cc`
- `src/platform/clock.h`
- `src/platform/clock.cc`
- `src/platform/mapped_read_limiter.h`

The original inventory contains 26 C++ definitions and three lambdas.
Filesystem mapping acquisition remains covered by the delivered N06/N07 slices;
this batch audits only the shared mapping-count primitive. Benchmarks remain out
of scope.

## Original definition ledger

| File | Definitions | Disposition |
|---|---:|---|
| `background_executor.h` | 10 | Six `BackgroundExecutor` ownership special members and four deleted `SerialExecutor` copy/move members are declarative ownership leaves. |
| `serial_executor.cc` | 4 | Constructor starts one worker. Destructor, scheduling and worker execution require named lifecycle children. |
| `clock.h` | 6 | `Clock` ownership special members are declarative interface leaves. |
| `clock.cc` | 2 | `Now` is a direct clock-query leaf. `SleepFor` composes stop priority, nonpositive admission and one cancellable positive wait. |
| `mapped_read_limiter.h` | 4 | Constructor validates the signed atomic domain; acquire, release and count conversion are atomic/accounting leaves. |

The three original lambdas are the worker entry, queue wait predicate and clock
wait predicate. The worker entry and predicates remain one-expression leaves.
All newly introduced helpers participate in the final ledger.

## Serial executor contracts

`Schedule` never executes a callback inline. It rejects an empty callable before
allocation, allocates the owned callable before taking the queue mutex, admits
or rejects it under that mutex, notifies after unlocking, and destroys a
rejected callable outside the mutex.

Accepted callbacks run in FIFO order on one worker. Waiting and queue removal
hold the queue mutex; callback execution and callback-object destruction do not.
A callback or its owned state may schedule more work.

Destruction closes admission and detaches all unstarted callbacks under the
queue mutex. It then requests stop, wakes and joins the worker. The running
callback receives the worker stop token and may finish cooperatively; detached
callbacks never run. Their owners are destroyed only after the worker has
joined and outside the queue mutex.

The implementation will name these operations:

1. enqueue one already-owned callback;
2. close admission and detach queued callbacks;
3. request stop, wake and join the worker;
4. wait for and remove one callback, with an empty owner representing stop;
5. execute the removed callback outside the queue mutex.

No public shutdown method, drain policy, exception swallowing or inline
execution is introduced.

## Clock contracts

A previously requested stop wins even for a nonpositive duration. Without a
stop request, a nonpositive duration succeeds without waiting. A positive wait
uses a stop-aware condition-variable wait and reports false when cancellation is
observed after the wait, otherwise true.

The local mutex protects no shared state; it exists only for the wait protocol.
The positive cancellable wait becomes a named leaf while `SleepFor` retains the
guard priority.

## Mapping-count contracts

The configured count must fit `std::ptrdiff_t` because a failed acquisition
temporarily decrements the atomic below zero before restoring it. Acquisition
uses one relaxed decrement, accepts only a positive previous count, and restores
every rejected attempt. Release is the balanced inverse for an admitted slot.

The limiter transfers no data between threads, so relaxed ordering remains
intentional. Balanced release is an internal ownership invariant; do not add a
runtime assertion or saturation policy.

## Characterization and validation

Before production edits:

- preserve the complete existing `SerialExecutorTest` and `SystemClockTest`
  identities;
- prove canceled callable owners are destroyed after the running callback
  finishes;
- add direct cross-platform limiter tests for capacity, rejected-attempt
  restoration, concurrent admission and signed-domain rejection;
- record the original selected JUnit identities.

After each responsibility-sized production commit, run the selected native
support tests. The final selected identity set must exactly match the
characterized set, and the complete project gates must pass on the reviewed PR
head.

## Final implementation ledger

The final implementation contains 31 definitions and the original three
lambdas:

| File | Final definitions | Reviewed responsibility |
|---|---:|---|
| `background_executor.h` | 10 | Declarative ownership and polymorphic scheduling boundaries. |
| `serial_executor.cc` | 8 | Worker start; shutdown composition; queue detachment; stop/wake/join; input validation; admission/notification; wait/dequeue; callback loop. |
| `clock.h` | 6 | Declarative clock ownership boundary. |
| `clock.cc` | 3 | Steady-clock query; positive cancellable wait; public stop/duration priority. |
| `mapped_read_limiter.h` | 4 | Signed-domain construction; atomic admission; balanced release; checked conversion. |

The worker-entry, queue-predicate and clock-predicate lambdas remain
single-expression leaves. No helper changes allocation timing, mutex ownership,
notification order, stop observation, error source context, callback destruction
or atomic ordering.

Selected Windows Debug characterization contains 17 exact identities: five
clock, three mapping limiter and nine serial executor tests. The characterized
and final identity sets are equal. The two new concurrency/lifecycle oracles
also pass 50 consecutive executions. GCC/Werror compiles the modified clock and
executor objects independently. The complete Windows Debug suite passes all 868
tests.

## Integrated review

Review rejected two details from the first extraction. Returning the detached
`std::deque` by value introduced a move that MSVC does not classify as
`noexcept`, so the final helper fills the destructor's existing local queue by
reference. Moving the shutdown error into `Enqueue` also changed its captured
function; the final call passes a `Schedule` source location explicitly and the
characterization checks the exact code, message, file and calling function.

## Review focus

Review lock release before every user-owned destructor, stop-before-duration
priority, queue ownership during returns, stop wake-up behavior, final task
destruction timing and atomic slot conservation. Reject helpers that merely
rename syntax or change allocation, error, notification or destruction order.
