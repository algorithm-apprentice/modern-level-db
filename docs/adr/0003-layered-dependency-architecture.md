# ADR-0003: Layered Dependency Architecture

- Status: Accepted
- Date: 2026-09-21

## Context

The original LevelDB file include graph is acyclic, but logical responsibilities
cross directory boundaries. In particular, the `db` directory contains both
low-level persistent formats and top-level orchestration, while table and DB
components depend on each other's concepts.

## Decision

The implementation will use the following strict dependency direction:

```text
api
 |
engine
 |
metadata
 |
table and memory
 |
format and platform
 |
base
```

The detailed graph and implementation nodes are defined in
`docs/dependency-dag.md`.

Lower layers must never include public API or engine headers. Persistent format
encoding must not perform filesystem I/O. Platform services must not know about
LSM concepts.

## Consequences

- Each node can be implemented and tested before higher-level orchestration
  exists.
- Some original LevelDB types must be split into format, metadata, policy, and
  orchestration types.
- Convenience dependencies that violate the direction must be replaced by
  narrower contracts rather than accepted as exceptions.
