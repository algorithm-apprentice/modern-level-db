# ADR-0005: Test-Driven Development

- Status: Accepted
- Date: 2026-09-21

## Context

A persistent storage engine can appear correct while containing latent defects
in malformed-input handling, ownership, synchronization, recovery ordering, or
snapshot visibility. Retrofitting tests after implementation makes it difficult
to distinguish specified behavior from accidental behavior.

Fast local feedback is also required. Crash loops, fuzzing, sanitizer builds,
and benchmarks are essential but are too expensive to run as the inner
development loop.

## Decision

All implementation nodes follow test-driven development:

1. Specify one observable behavior in a focused unit test.
2. Run the test and observe the expected failure.
3. Add the smallest production implementation that satisfies the behavior.
4. Run the focused test and the complete fast unit-test suite.
5. Refactor only while all tests remain green.

Unit tests are part of the implementation node and are never deferred to a
separate testing phase.

Every module test suite must cover, where applicable:

- Normal behavior.
- Empty and boundary values.
- Malformed or truncated input.
- Overflow and size-limit handling.
- Ownership and lifetime contracts.
- Deterministic failure injection.
- Compatibility vectors for persistent formats.

Tests are divided into execution tiers:

- `unit`: deterministic and fast enough for every local change.
- `model`: randomized state-machine tests with deterministic seeds.
- `compatibility`: golden and differential tests against upstream LevelDB.
- `crash`: durability and fault-injection scenarios.
- `fuzz`: parser and stateful fuzz targets.
- `benchmark`: performance and allocation baselines.

Only the `unit` tier is mandatory in the inner red-green-refactor loop. CI runs
the broader tiers according to their cost.

## Consequences

- Production interfaces are designed for observability and dependency
  injection from the beginning.
- A DAG node cannot be marked complete based only on compilation.
- Unit tests must remain focused and avoid sleeps, large datasets, or expensive
  process setup.
- Persistent-format implementation starts from golden vectors rather than from
  unverified encoder output.
