# ADR-0010: Need-Driven Simplicity

- Status: Accepted
- Date: 2026-09-22

## Context

Storage engines already contain unavoidable complexity in durability,
recovery, concurrency, and compaction. General-purpose frameworks, speculative
extension points, and support for hypothetical callers make those critical
paths harder to reason about and test.

Modern LevelDB targets a small embedded ordered key-value engine. It does not
aim to become a generic runtime, filesystem framework, or feature-complete
RocksDB replacement.

## Decision

Design the smallest complete solution for the current Modern LevelDB
requirements.

Before adding an abstraction or capability, identify:

- The concrete current caller.
- The operation and invariant it requires.
- Its ownership and lifetime.
- Its concurrency and failure model.
- The test or measurement that will verify the requirement.

Do not add behavior solely because a reusable library might conventionally
provide it. In particular, avoid unsupported:

- Generic lifecycle protocols and reusable state machines.
- Plug-in systems, policy frameworks, and configuration knobs.
- Parallelism, lock-free data structures, and asynchronous APIs.
- Migration and compatibility machinery for formats with no deployed data.
- Public interfaces broader than the engine's actual call sites.

Prior art remains the starting point, but Modern LevelDB adopts only the subset
needed by its current consumers. A mature solution's extra generality is not
automatically a project requirement.

Additional complexity is justified only by at least one of:

- A current functional requirement.
- A correctness or safety invariant that the simpler design cannot satisfy.
- A concrete testing seam needed for deterministic verification.
- Repeated implemented call sites that would otherwise duplicate logic.
- A measured performance bottleneck with evidence that the proposed mechanism
  addresses it.

When requirements grow, refactor at that time with tests and an ADR. Do not
prepay complexity for a hypothetical future migration.

## Review rule

Design and code review must ask:

1. Which current Modern LevelDB path uses this behavior?
2. What breaks if it is removed?
3. Can upstream LevelDB's simpler mechanism satisfy the requirement?
4. Is the implementation supporting callers or races that the ownership model
   explicitly forbids?
5. Is the complexity proportional to the likelihood and impact of the problem?

If there is no concrete answer, remove or defer the capability.

## Consequences

- Some internal components intentionally expose fewer operations than
  general-purpose libraries.
- Future requirements may require refactoring rather than flipping a
  pre-designed extension point.
- Smaller contracts reduce states, tests, synchronization paths, and review
  surface.
- Performance optimizations follow measurement after the relevant engine path
  exists.

## Example

`SerialExecutor` is owned exclusively by one future DB instance. The owner
prevents new scheduling and destroys the executor before callback-visible DB
state. Therefore it needs thread-safe task submission and RAII destruction,
but not a public concurrent shutdown protocol, restart, or reuse after
stopping.
