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

New or changed executable production behavior follows test-driven development:

1. Specify one observable behavior in a focused unit test.
2. Run the test and observe the expected failure.
3. Add the smallest production implementation that satisfies the behavior.
4. Run the focused test and the complete fast unit-test suite.
5. Refactor only while all tests remain green.

Unit tests are part of each production-code node and are never deferred to a
separate testing phase. Behavior-preserving refactors start from a green
baseline and keep it green; they do not require an artificial failing test.

Build-system changes use appropriate configure, build, and test-integration
checks, reproducing the failure before fixing a configuration bug.
Documentation-only changes are reviewed for accuracy, consistency, references,
and dependency ordering rather than subjected to unit tests with no executable
behavior.

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
- `cmake`: isolated consumer-configuration checks for build integration.
- `model`: randomized state-machine tests with deterministic seeds.
- `compatibility`: golden and differential tests against upstream LevelDB.
- `crash`: durability and fault-injection scenarios.
- `fuzz`: parser and stateful fuzz targets.
- `benchmark`: performance and allocation baselines.

The `unit` tier is the inner loop for production-code changes; build-system
changes run the relevant `cmake` checks. CI runs the broader tiers according to
their cost. The bootstrap currently runs `unit` and `cmake` in CI; the remaining
tiers are introduced with the components they exercise.

CI additionally requires the `unit` tier to execute every added or modified
production line and branch, as defined by
[ADR-0019](0019-test-coverage-policy.md). Coverage complements, and never
replaces, behavioral assertions.

## Consequences

- Production interfaces are designed for observability and dependency
  injection from the beginning.
- A DAG node cannot be marked complete based only on compilation.
- Unit tests must remain focused and avoid sleeps, large datasets, or expensive
  process setup.
- Persistent-format implementation starts from golden vectors rather than from
  unverified encoder output.
