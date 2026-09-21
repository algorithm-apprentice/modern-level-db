# Modern LevelDB

Modern LevelDB is a ground-up C++23 reimplementation of the core LevelDB
storage model. The project aims to preserve LevelDB's small, ordered,
single-process key-value semantics while rebuilding the implementation around
explicit ownership, testable durability boundaries, and a strict acyclic
architecture.

The project is currently pre-alpha and is not suitable for production data.

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

Development follows test-driven development. Every implementation change starts
with a focused failing unit test, proceeds to the smallest correct
implementation, and is refactored only while the test suite remains green.

Development is delivered through sequential pull requests. The next DAG slice
does not begin until the current pull request has been reviewed and merged.

All source code, identifiers, comments, documentation, ADRs, and commit
messages are written in English.

## Upstream reference

The original [Google LevelDB](https://github.com/google/leveldb) implementation
is used as a behavioral oracle, file-format reference, and differential testing
target. Modern LevelDB is not a line-by-line translation.

## License

Modern LevelDB is distributed under the BSD 3-Clause License. See
[LICENSE](LICENSE).
