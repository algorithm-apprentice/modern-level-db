# Modern LevelDB

Modern LevelDB is a ground-up C++23 implementation of the core LevelDB
storage model. It preserves the ordered embedded key-value semantics and
compatible WAL, MANIFEST, and SSTable formats while using explicit ownership,
typed errors, testable durability boundaries, and an acyclic architecture.

> **Status:** pre-alpha. Use disposable databases only; this project is not
> suitable for production data.

## What is implemented

- Ordered binary `Put`, `Get`, and `Delete`.
- Atomic write batches, snapshots, and bidirectional iteration.
- LevelDB-compatible recovery, WAL/MANIFEST metadata, and SSTables.
- Bloom filters plus None, Snappy, and Zstd block compression.
- Mutable/immutable memtables, grouped writes, background flush, leveled and
  seek-triggered compaction, and bounded table/block caches.
- Typed owning database-state inspection and an offline storage diagnostic
  command.
- Native Linux/macOS POSIX storage and an admitted Windows 10/11 desktop
  boundary for x64 MSVC on local fixed NTFS.
- Model, compatibility, crash/process, sanitizer, fuzz, benchmark, and
  profiling evidence.

Modern LevelDB is not source- or ABI-compatible with the original LevelDB C++
API. See the
[LevelDB comparison](docs/learning/11-leveldb-comparison.md) for current API
differences and deliberately deferred features.

## Quick start

Install a C++23 compiler, CMake 3.25 or newer, and Ninja. The complete
prerequisites and consumption instructions are in
[Getting started](docs/reference/getting-started.md).

Linux/macOS:

```console
cmake --preset dev-debug
cmake --build --preset dev-debug
ctest --preset dev-debug
```

Windows requires an initialized **x64 MSVC developer environment**:

```console
cmake --preset windows-debug
cmake --build --preset windows-debug
ctest --preset windows-debug
```

Native Windows database opening is disabled until every open explicitly sets
`Options::allow_weak_namespace_durability = true`. That consent admits weaker
namespace persistence; it does not provide POSIX-equivalent power-loss
durability. Read the
[platform support and durability contract](docs/reference/platform-support-and-durability.md)
before opting in.

Consumers link `modern_leveldb::modern_leveldb` and include
`modern_leveldb/db.h`. The
[API and options reference](docs/reference/api-and-options.md) defines the
public defaults, clipping rules, ownership, and lifetime behavior.

## Safety boundaries

- One process exclusively owns an open database directory.
- Do not externally modify, replace, or truncate live database files, run the
  offline diagnostic against them, or redirect command output onto them.
- Default read-only mappings can terminate the process on a storage fault.
  Set `allow_mmap_reads = false` when typed read errors are required.
- Native Windows support is limited to the admitted x64/MSVC/local-fixed-NTFS
  boundary and requires weak-namespace consent.
- The diagnostic tool is offline and read-only. Its escaped text may contain
  application keys and values; it is not backup, restore, repair, or
  whole-file certification.

The full operational contract is in the
[user reference](docs/reference/README.md).

## Documentation

| Need | Start here |
|---|---|
| Build, consume, configure, and use the library safely | [User reference](docs/reference/README.md) |
| Build tests, understand CI, benchmark, profile, and follow style | [Development guide](docs/development/README.md) |
| Understand modules, dependency direction, and runtime flows | [Architecture](docs/architecture.md) |
| Learn the engine through concepts, source tours, and runnable labs | [Learning path](docs/learning/README.md) |
| Review decisions, outcomes, amendments, and supersession | [ADR index](docs/adr/README.md) |
| Follow delivery dependencies and completed roadmap nodes | [Delivery DAG](docs/dependency-dag.md) |

## Project scope

The project aims to remain a small, understandable embedded LSM engine with
first-class crash consistency, fault injection, fuzzing, differential tests,
and reproducible measurement. Abstractions are added for concrete callers,
not speculative generality.

Current non-goals include:

- distributed storage, replication, SQL, or a client-server protocol;
- transactions beyond atomic write batches;
- column families, merge operators, remote storage, or a general plugin
  framework;
- source or binary compatibility with the original LevelDB API;
- production certification or stronger guarantees outside the documented
  platform boundaries.

## Upstream reference

[Google LevelDB](https://github.com/google/leveldb/tree/7ee830d02b623e8ffe0b95d59a74db1e58da04c5)
revision `7ee830d02b623e8ffe0b95d59a74db1e58da04c5` is the behavioral oracle,
file-format reference, and differential-testing target. Modern LevelDB is an
independent implementation, not a line-by-line translation.

## License

Modern LevelDB is distributed under the BSD 3-Clause License. See
[LICENSE](LICENSE).
