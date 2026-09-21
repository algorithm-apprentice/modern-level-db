# ADR-0001: Ground-Up C++23 Reimplementation

- Status: Accepted
- Date: 2026-09-21

## Context

The original LevelDB codebase is small and proven, but its public ownership
model, platform abstraction, background-work model, and source layout reflect
compatibility constraints accumulated since 2011.

Modern LevelDB has no deployed users, existing databases, or binary
compatibility commitments.

## Decision

Modern LevelDB will be implemented from an empty source tree using C++23.

The original Google LevelDB repository will be used as:

- A behavioral specification.
- A persistent-format reference.
- A source of test scenarios and golden vectors where licensing and
  attribution requirements are preserved.
- A differential testing oracle.

The new implementation will not be a line-by-line translation and will not
retain the original class or directory structure when those structures conflict
with the target dependency architecture.

## Consequences

- We can design explicit ownership and dependency boundaries without legacy API
  constraints.
- Production readiness requires substantially more validation than successful
  feature implementation.
- Behavioral and format differences must be intentional, documented, and
  covered by ADRs.
- The project may remain pre-alpha for a significant period while recovery and
  crash-consistency behavior is validated.
