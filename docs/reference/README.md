# User Reference

This directory is the current user-facing reference for Modern LevelDB.
Architecture decision records explain history and rationale; the pages here
describe how the current library behaves and how to use it safely.

Modern LevelDB is pre-alpha. Use disposable databases and keep independent
backups of any data that matters.

## Start here

| Need | Document |
|---|---|
| Build and consume the library | [Getting started](getting-started.md) |
| Understand the public facade and options | [API and options](api-and-options.md) |
| Choose a supported platform and durability policy | [Platform support and durability](platform-support-and-durability.md) |
| Inspect WAL, MANIFEST, and SSTable files offline | [Storage diagnostics](storage-diagnostics.md) |

## Authority and history

Public headers are the compile-time API contract:

- [`db.h`](../../include/modern_leveldb/db.h)
- [`options.h`](../../include/modern_leveldb/options.h)
- [`iterator.h`](../../include/modern_leveldb/iterator.h)
- [`snapshot.h`](../../include/modern_leveldb/snapshot.h)
- [`write_batch.h`](../../include/modern_leveldb/write_batch.h)
- [`database_state.h`](../../include/modern_leveldb/database_state.h)

The [architecture guide](../architecture.md) explains implementation
boundaries. The [learning path](../learning/README.md) teaches the engine.
The [ADR index](../adr/) contains decision history; until its dedicated index
is delivered, use the numbered files in that directory.

## Safety summary

- Do not open the same database directory more than once, whether from one
  process or several processes.
- Do not modify, replace, truncate, or redirect output onto live database
  files.
- Read-only mappings can terminate the process on storage faults. Disable
  `allow_mmap_reads` when typed read errors are required.
- Native Windows database opening requires explicit weak-namespace consent.
  File sync does not create POSIX-equivalent directory durability there.
- The diagnostic tool is offline and read-only; its output may contain
  sensitive application keys and values.
