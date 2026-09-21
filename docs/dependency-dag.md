# Implementation Dependency DAG

## Purpose

Modern LevelDB is implemented as a directed acyclic graph of independently
verifiable modules. Dependency edges represent compile-time or semantic
prerequisites, not scheduling preferences.

Only one implementation node is developed at a time. Independent ready nodes
remain pending until the current node is complete.

## Layer graph

```mermaid
flowchart TD
  API[api]
  ENGINE[engine]
  METADATA[metadata]
  TABLE[table]
  MEMORY[memory]
  FORMAT[format]
  PLATFORM[platform]
  BASE[base]

  API --> ENGINE
  ENGINE --> METADATA
  ENGINE --> TABLE
  ENGINE --> MEMORY
  ENGINE --> FORMAT
  ENGINE --> PLATFORM
  METADATA --> TABLE
  METADATA --> FORMAT
  METADATA --> PLATFORM
  TABLE --> FORMAT
  TABLE --> PLATFORM
  TABLE --> BASE
  MEMORY --> FORMAT
  MEMORY --> BASE
  FORMAT --> BASE
  PLATFORM --> BASE
```

## Nodes and direct dependencies

| Node | Deliverable | Direct dependencies |
|---|---|---|
| `document-architecture` | Architecture analysis, DAG, and ADRs | None |
| `bootstrap-build` | C++23 CMake and CTest foundation | `document-architecture` |
| `implement-bytes` | Byte views and conversions | `bootstrap-build` |
| `implement-error-result` | Typed errors and `Result<T>` | `bootstrap-build` |
| `implement-comparator` | Bytewise comparator contract | `implement-bytes` |
| `implement-coding` | Fixed-width and varint coding | `implement-bytes`, `implement-error-result` |
| `implement-checksum-hash` | CRC32C and stable hash | `implement-coding` |
| `implement-arena` | Monotonic arena | `bootstrap-build` |
| `implement-platform-runtime` | Clock and background executor | `implement-error-result` |
| `implement-platform-fs` | Filesystem and file interfaces | `implement-bytes`, `implement-error-result` |
| `implement-internal-key` | Internal-key format and ordering | bytes, result, coding, comparator |
| `implement-wal-format` | WAL physical-record format | bytes, result, coding, checksum |
| `implement-cache` | Sharded RAII block cache | bytes, result, checksum/hash |
| `implement-skiplist` | Concurrent-read skip list | arena |
| `implement-write-batch` | Atomic batch format | internal key, coding, result |
| `implement-memtable` | Arena-backed mutable table | internal key, arena, skip list |
| `implement-wal-io` | WAL reader and writer | filesystem, WAL format |
| `implement-filenames` | Database filename model | result |
| `implement-version-edit` | MANIFEST edit format | internal key, coding, result |
| `implement-block-format` | Data blocks, handles, footer, trailers | bytes, result, coding, checksum, comparator |
| `implement-filter` | Bloom and filter blocks | bytes, checksum/hash |
| `implement-sstable-writer` | SSTable construction | filesystem, block format, filter |
| `implement-sstable-reader` | SSTable reads and iteration | filesystem, block format, filter, cache |
| `implement-table-cache` | Cached SSTable handles | SSTable reader, cache, filenames |
| `implement-version-set` | Versions and MANIFEST state | version edit, filenames, SSTable reader, WAL I/O |
| `implement-recovery` | MANIFEST and WAL recovery | WAL I/O, write batch, memtable, version set |
| `implement-read-path` | Point reads and merged iteration | memtable, table cache, version set |
| `implement-write-path` | Group commit and memtable insertion | WAL I/O, write batch, memtable, version set |
| `implement-flush` | Immutable memtable to L0 | memtable, SSTable writer, version set |
| `implement-compaction` | Leveled compaction | SSTable reader/writer, version set |
| `implement-db-engine` | Integrated DB lifecycle | recovery, read/write, flush, compaction, runtime |
| `implement-public-api` | Public RAII C++ API | DB engine |
| `build-compatibility-harness` | Model, golden, differential, crash, and fuzz tests | public API |
| `harden-engine` | Sanitizer, crash, fuzz, and benchmark gates | compatibility harness |

## Canonical topological order

The DAG allows more than one valid topological sort. The project uses this
canonical order to keep development sequential and reviewable:

1. `document-architecture`
2. `bootstrap-build`
3. `implement-bytes`
4. `implement-error-result`
5. `implement-comparator`
6. `implement-coding`
7. `implement-checksum-hash`
8. `implement-arena`
9. `implement-platform-runtime`
10. `implement-platform-fs`
11. `implement-internal-key`
12. `implement-wal-format`
13. `implement-cache`
14. `implement-skiplist`
15. `implement-write-batch`
16. `implement-memtable`
17. `implement-wal-io`
18. `implement-filenames`
19. `implement-version-edit`
20. `implement-block-format`
21. `implement-filter`
22. `implement-sstable-writer`
23. `implement-sstable-reader`
24. `implement-table-cache`
25. `implement-version-set`
26. `implement-recovery`
27. `implement-read-path`
28. `implement-write-path`
29. `implement-flush`
30. `implement-compaction`
31. `implement-db-engine`
32. `implement-public-api`
33. `build-compatibility-harness`
34. `harden-engine`

## Completion rule

A node is complete only when:

- Its observable behavior is first expressed by a failing unit test.
- The failing test is observed before production implementation begins.
- The smallest implementation needed to make the new test pass is added.
- Refactoring occurs only after the focused and existing unit tests are green.
- Its public contract is documented.
- Its dependency direction follows the architecture rules.
- Focused tests cover successful, boundary, malformed, and failure behavior.
- Sanitizer-compatible code contains no owning raw pointers.
- Persistent-format nodes have LevelDB golden-vector tests.
- Performance-sensitive nodes have a baseline before optimization.

Algorithm changes are not combined with unrelated ownership or API refactors.
