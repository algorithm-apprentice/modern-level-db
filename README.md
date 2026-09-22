# Modern LevelDB

Modern LevelDB is a ground-up C++23 reimplementation of the core LevelDB
storage model. The project aims to preserve LevelDB's small, ordered,
single-process key-value semantics while rebuilding the implementation around
explicit ownership, testable durability boundaries, and a strict acyclic
architecture.

The project is currently pre-alpha and is not suitable for production data.

The implemented foundation covers byte views, typed results, comparison,
binary coding, CRC32C, LevelDB-compatible seeded hashing, and a monotonic
arena. The platform foundation also provides a steady clock and a serial,
stop-aware background executor, portable file interfaces, and the first
Linux/macOS POSIX filesystem backend. Database operations, WAL/SSTable
persistence, and compaction are not yet implemented.

## Goals

- Provide an embeddable ordered key-value store with `Put`, `Get`, `Delete`,
  atomic write batches, snapshots, and ordered iteration.
- Use modern C++ ownership and error handling without exceptions in normal
  storage-engine control flow.
- Preserve the LevelDB v1 WAL, MANIFEST, and SSTable formats during the first
  implementation phase.
- Make crash consistency, fault injection, fuzzing, and differential testing
  first-class engineering constraints.
- Keep the implementation substantially smaller and more approachable than a
  feature-complete RocksDB-style engine.
- Design abstractions only for concrete Modern LevelDB call sites; do not add
  general-purpose capabilities or complexity without a current requirement.

## Non-goals

- Distributed storage, replication, or a client-server protocol.
- SQL, secondary indexes, or a relational data model.
- Transactions beyond atomic write batches in the initial implementation.
- Column families, merge operators, remote storage, or a general plugin
  framework.
- Source or binary compatibility with the original LevelDB C++ API.

## Architecture

- [LevelDB architecture analysis](docs/architecture.md)
- [Implementation dependency DAG](docs/dependency-dag.md)
- [Architecture decision records](docs/adr/)

## Development

The project uses C++23, CMake, Ninja, CTest, and GoogleTest.

```bash
cmake --preset dev-debug
cmake --build --preset dev-debug
ctest --preset dev-debug -L unit
```

Tests are enabled by default for a standalone build and disabled when the
library is added as a subproject. Either `MODERN_LEVELDB_BUILD_TESTS=OFF` or
`BUILD_TESTING=OFF` disables test targets and their dependencies. When tests
are explicitly enabled by a parent project, its GoogleTest options are
preserved.

Run `ctest --preset dev-debug` to include the `cmake` consumer-configuration
checks as well as the fast `unit` suite.

New or changed production behavior follows test-driven development: start with
a focused failing unit test, implement the smallest correct change, and
refactor while tests remain green. Configuration and documentation changes use
the applicable checks defined in [ADR-0005](docs/adr/0005-test-driven-development.md).

Development is delivered through sequential pull requests. The next DAG slice
does not begin until the current pull request has been reviewed and merged.
Before owner review, independent personas review the slice, each finding is
evaluated against evidence, and accepted fixes receive follow-up review as
described in [ADR-0006](docs/adr/0006-sequential-pull-request-workflow.md).

All source code, identifiers, comments, documentation, ADRs, and commit
messages are written in English.

See [Code style](docs/code-style.md) for naming, formatting, and review
conventions.

## Upstream reference

The original [Google LevelDB](https://github.com/google/leveldb) implementation
is used as a behavioral oracle, file-format reference, and differential testing
target. Modern LevelDB is not a line-by-line translation.

## License

Modern LevelDB is distributed under the BSD 3-Clause License. See
[LICENSE](LICENSE).
