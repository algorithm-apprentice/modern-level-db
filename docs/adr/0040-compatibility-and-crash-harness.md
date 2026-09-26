# ADR-0040: Compatibility, Crash, and Fuzz Harness

## Status

Accepted

## Context and prior art

The public API and compression nodes complete the initial engine features.
Earlier nodes have unit, binary golden, and file-failure tests, but their
cross-implementation executables live only in development sessions.
`build-compatibility-harness` makes end-to-end verification reproducible from
the repository without adding production APIs.

Google LevelDB's `db/fault_injection_test.cc` tracks file sync positions,
freezes filesystem state at a simulated crash, and drops unsynced contents
and names. SQLite's in-memory crash VFS similarly captures persistence state
at successive I/O boundaries and checks recovery afterward. We adopt bounded
deterministic enumeration, not a general filesystem simulator or claims
about every hardware failure.

LLVM libFuzzer provides in-process coverage-guided generation and persistent
crash reproducers. It complements rather than replaces fixed golden vectors
and state-machine oracles.

## Decision

### Build and tier boundaries

`MODERN_LEVELDB_BUILD_EXTENDED_TESTS=ON`, default off, adds the `model`,
`compatibility`, and `crash` CTest tiers. These use the existing GoogleTest
dependency and require the supported POSIX backend. Ordinary builds still
run only `unit` and `cmake`.

`MODERN_LEVELDB_BUILD_FUZZERS=ON`, default off, adds parser and database
libFuzzer executables. It requires a Clang toolchain with the libFuzzer
runtime; configuration fails explicitly if the runtime is missing.
The targets instrument the production library, not merely the test driver.
Only a fuzz build links the fuzzer entry-point runtime. Ordinary unit
executables keep their existing main.

Add reproducible presets for the extended harness and fuzz build. Keep
dependency setup explicit and reuse codec targets. The reference LevelDB
revision remains `7ee830d02b623e8ffe0b95d59a74db1e58da04c5`; it is fetched only for
extended tests and built without modifying upstream source. Its CMake link
properties refer to the already-selected codec targets, so upstream's
bare `snappy`/`zstd` library names do not require system packages.
Set `HAVE_SNAPPY=1` and `HAVE_ZSTD=1` in the reference configuration scope
before upstream runs its configure-time probes: changing link properties
alone cannot restore codec branches that preprocessing disabled. Parent
values remain unchanged. A runtime table-write check verifies both codecs.

### Ordered-map model

A seeded operation stream uses binary keys, compressible and noncompressible
values, overwrites, deletions, atomic multi-operation batches, snapshots,
point reads, scans in both directions, seeks, and reopenings. An independent
`std::map<std::string, std::string>` tracks visible state, with copies for
snapshot state. Assertions report the seed and operation index.

Reopening releases every iterator and snapshot first. Snapshot operations
never use released handles. Tests select each compression mode and a small
write buffer so that reads traverse WAL recovery and flushed tables as well
as the active memtable.

### Golden and differential compatibility

Keep the existing fixed WAL, MANIFEST, coding, and table vectors in `unit`.
The compatibility tier adds a small frozen upstream database image, expressed
as file-name/hex-content pairs with its reference revision and logical
contents documented. Tests open a fresh copy; the original fixture is never
modified.

An upstream differential test applies the same seeded operations to Modern
LevelDB and Google LevelDB and compares results to the model, not just to each
other. It includes snapshots, binary data, scans, batches, and all compression
modes. It then closes both databases, opens each implementation's directory
with the other implementation, and checks every visible key again.

Physical table layouts and compressed encodings are not required to be
identical: codec versions and compaction scheduling may differ. Logical
contents, snapshot visibility, and interoperable persisted data are required.
The reference's codec support is checked; an unavailable codec fails the test
instead of silently turning the comparison into an uncompressed test.

### Simulated power loss

Add a test-only `CrashFileSystem` over the existing `MemoryFileSystem`.
Separate file identity from directory names:

- A writable handle owns a file-state reference. `Sync` records durable bytes;
  `Append`, `Flush`, and `Close` alone do not make them durable.
- A live name map and a durable name map hold file-state references.
  `SyncDirectory` publishes current child file and directory names.
- Rename moves the live reference, preserving an older durable destination
  until its directory is synced. Unsynced removal leaves the durable name.
- A crash freezes mutation. Cleanup and error unwinding cannot subsequently
  make data durable.
- Exporting a crash image copies only durable names and their durable bytes;
  a new filesystem and engine recover from that image.

The simulator supports the engine's current create-new-file/append/rename
workflow. It does not pretend to model arbitrary open-handle unlink behavior,
mmap, disk sectors, or write reordering. Its own focused tests establish
file-sync versus directory-sync behavior and rename/removal rollback.

Start from an established database, enumerate every mutating I/O boundary
through recovery, synced batches, WAL rotation, flushes, compactions, and
shutdown, and recover each frozen image independently. Every acknowledged
sync batch must survive. Recovery may include a whole in-flight batch, but
never a partial batch. A final asynchronous batch may be absent. The oracle
compares the recovered map with allowed complete transaction states rather
than assuming every attempted write committed.

A manually drained executor makes each trace repeatable and avoids timing
sleeps. All accepted tasks are drained before destroying the test engine,
even on injected failure. This tests persistence rather than scheduler luck.

### Fuzz targets

The format target exercises WAL physical records, write-batch payloads,
internal keys, version edits, sorted blocks, footers, and stored compressed
blocks. Valid decodes are re-encoded and decoded again where an encoder
exists. Inputs are bounded; codec-advertised output beyond the fuzz memory
budget is skipped, without changing the production format's size limit.

The stateful target uses the injected in-memory engine and an independent
map. Bounded operations cover writes, batches, deletions, reads, snapshots,
iteration, background work, and reopening. Each invocation owns and destroys
its filesystem, executor, handles, and model; no state leaks across inputs.

Fuzzer mismatches abort so libFuzzer retains the input. Fixed corpus replay
and deterministic smoke runs are available separately from longer campaigns.
Time, input-length, and RSS limits are explicit; reaching a time limit is not
described as exhaustive verification.

## Validation and scope

Verify the test infrastructure itself, including a power-loss image that
omits unsynced bytes and one that retains a replaced durable name. Exercise
all added tiers and the reference codec checks. The later `harden-engine`
node adds the final sanitizer, longer-running crash/fuzz, and benchmark
gates; it does not defer basic harness execution.

No repair API, migration machinery, Windows filesystem, production fault
injection, filesystem abstraction redesign, or generic test DSL is added.
Any real engine defect discovered by the harness receives a focused unit
regression and the existing changed-code coverage gate.

## References

- [Google LevelDB fault injection](https://github.com/google/leveldb/blob/7ee830d02b623e8ffe0b95d59a74db1e58da04c5/db/fault_injection_test.cc)
- [SQLite crash testing](https://www.sqlite.org/testing.html#crash_testing)
- [LLVM libFuzzer](https://llvm.org/docs/LibFuzzer.html)
