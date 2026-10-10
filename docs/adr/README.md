# Architecture decision records

This index provides searchable navigation across the project's architecture
decision records (ADRs). The status block at the top of each ADR is the
canonical lifecycle authority; this page is a derived summary, not a second
status source.

Lifecycle labels follow
[ADR-0070](0070-documentation-information-architecture.md): `Proposed`,
`Accepted`, `Implemented`, `Measurement-only`, `Rejected`, and `Superseded`.
An accepted ADR can also name later amendments without making the complete
decision obsolete.

The documentation manifest uses `current` and `historical` for
**document-retention lifecycle**. A historical decision record is retained
history; that manifest value does not mean its decision was rejected or
superseded. The exact `- Status:` field near the top of each ADR is the sole
decision-lifecycle authority and must match this index.

For current operational behavior, use the linked
[user reference](../reference/README.md),
[development guide](../development/README.md), and
[architecture description](../architecture.md). ADR bodies preserve the
decision context and evidence available when each change was made.

## Foundation and core structures

| ADR | Topic | Lifecycle | Implementation / outcome | Supersession / current reference |
|---|---|---|---|---|
| [ADR-0001: Ground-Up C++23 Reimplementation](0001-ground-up-cpp23-reimplementation.md) | Project boundary | Accepted | Establishes the independent C++23 implementation | [Architecture](../architecture.md) |
| [ADR-0002: Initial LevelDB Format Compatibility](0002-leveldb-format-compatibility.md) | Compatibility | Accepted | Establishes the persistent-format oracle | [Platform and durability](../reference/platform-support-and-durability.md) |
| [ADR-0003: Layered Dependency Architecture](0003-layered-dependency-architecture.md) | Architecture | Accepted | Establishes dependency direction | [Current architecture](../architecture.md) |
| [ADR-0004: Errors, Ownership, and Runtime Model](0004-errors-ownership-and-runtime.md) | Runtime model | Accepted | Establishes status, ownership, and exception boundaries | [API and options](../reference/api-and-options.md) |
| [ADR-0005: Test-Driven Development](0005-test-driven-development.md) | Engineering process | Accepted | Establishes test-first delivery | [Building and testing](../development/building-and-testing.md) |
| [ADR-0006: Sequential Pull Request Workflow](0006-sequential-pull-request-workflow.md) | Delivery process | Accepted | Establishes sequential reviewed slices | [CI and quality gates](../development/ci-and-quality-gates.md) |
| [ADR-0007: Checksum and Hash Contracts](0007-checksum-and-hash-contracts.md) | Checksums | Accepted | Implements shared checksum/hash contracts | [Current architecture](../architecture.md) |
| [ADR-0008: Monotonic Arena](0008-monotonic-arena.md) | Memory management | Accepted | Implements monotonic allocation | [Current architecture](../architecture.md) |
| [ADR-0009: Platform Runtime Services](0009-platform-runtime-services.md) | Platform abstraction | Accepted | Establishes platform service interfaces | [Platform and durability](../reference/platform-support-and-durability.md) |
| [ADR-0010: Need-Driven Simplicity](0010-need-driven-simplicity.md) | Design policy | Accepted | Establishes scope discipline | [Current architecture](../architecture.md) |
| [ADR-0011: Filesystem Contracts and POSIX Backend](0011-filesystem-contracts.md) | Filesystem | Accepted | Implements the POSIX backend and durability contract | [ADR-0064](0064-windows-filesystem-and-delivery.md); [platform and durability](../reference/platform-support-and-durability.md) |
| [ADR-0012: Internal Key Format](0012-internal-key-format.md) | Key encoding | Accepted | Implements the internal-key format | [Current architecture](../architecture.md) |
| [ADR-0013: WAL Record Format](0013-wal-record-format.md) | WAL format | Accepted | Physical framing remains current; production fragmentation was streamlined | [ADR-0060](0060-leveldb-write-path-parity.md) |
| [ADR-0014: Typed Sharded LRU Cache](0014-sharded-lru-cache.md) | Cache | Accepted | Establishes the typed cache contract | [ADR-0054](0054-leveldb-cache-parity.md) |
| [ADR-0015: Single-Writer Concurrent-Reader Skip List](0015-concurrent-skiplist.md) | Memtable index | Accepted | Implements the skip-list concurrency model | [ADR-0060](0060-leveldb-write-path-parity.md) |
| [ADR-0016: Write Batch Format and Reader](0016-write-batch-format.md) | Write batches | Accepted | Persistent encoding remains current; private storage changed | [ADR-0060](0060-leveldb-write-path-parity.md); [API and options](../reference/api-and-options.md) |
| [ADR-0017: Arena-Backed MemTable](0017-arena-backed-memtable.md) | Memtable | Accepted | Implements arena-backed mutable state | [ADR-0060](0060-leveldb-write-path-parity.md) |
| [ADR-0018: WAL Stream Reader and Writer](0018-wal-stream-io.md) | WAL I/O | Accepted | Implements streaming WAL I/O; later work refines the writer path | [ADR-0060](0060-leveldb-write-path-parity.md) |

## Database engine delivery

| ADR | Topic | Lifecycle | Implementation / outcome | Supersession / current reference |
|---|---|---|---|---|
| [ADR-0019: Test Coverage Policy](0019-test-coverage-policy.md) | Quality gates | Accepted | Establishes changed-code coverage policy | [CI and quality gates](../development/ci-and-quality-gates.md) |
| [ADR-0020: Database File Names](0020-database-file-names.md) | Storage layout | Accepted | Implements LevelDB-compatible names and parsing | [Storage diagnostics](../reference/storage-diagnostics.md) |
| [ADR-0021: MANIFEST Version Edits](0021-manifest-version-edits.md) | MANIFEST format | Accepted | Implements version-edit encoding | [Current architecture](../architecture.md) |
| [ADR-0022: SSTable Block Format](0022-sstable-block-format.md) | SSTable format | Accepted | Implements block encoding and decoding | [ADR-0055](0055-leveldb-block-iterator-parity.md) |
| [ADR-0023: SSTable Filter Blocks](0023-sstable-filter-blocks.md) | SSTable filters | Accepted | Implements filter-block compatibility | [Current architecture](../architecture.md) |
| [ADR-0024: SSTable Writer](0024-sstable-writer.md) | SSTable writing | Accepted | Implements compatible table generation | [ADR-0060](0060-leveldb-write-path-parity.md) |
| [ADR-0025: SSTable Reader](0025-sstable-reader.md) | SSTable reading | Accepted | Implements table lookup and iteration with later lazy-decoding and ownership changes | [ADR-0055](0055-leveldb-block-iterator-parity.md); [ADR-0056](0056-leveldb-table-mmap-parity.md); [ADR-0057](0057-leveldb-version-output-parity.md) |
| [ADR-0026: Table Cache](0026-table-cache.md) | Table cache | Accepted | Implements cached table access | [ADR-0054](0054-leveldb-cache-parity.md); [ADR-0056](0056-leveldb-table-mmap-parity.md) |
| [ADR-0027: Versions and the Version Set](0027-version-set.md) | Version management | Accepted | Implements version state and MANIFEST application | [ADR-0057](0057-leveldb-version-output-parity.md) |
| [ADR-0028: Level-0 Table Building](0028-level0-table-building.md) | Table building | Accepted | Shared table building remains current; recovery writes L0 while ordinary flushes select a level | [ADR-0033](0033-memtable-flush.md); [ADR-0060](0060-leveldb-write-path-parity.md) |
| [ADR-0029: Database Recovery](0029-database-recovery.md) | Recovery | Accepted | Implements open, replay, and recovery | [ADR-0065](0065-windows-recovery-and-compatibility-verification.md) |
| [ADR-0030: Point Reads](0030-point-reads.md) | Read path | Accepted | Implements point lookup; later parity work changes visitation and output ownership | [ADR-0057](0057-leveldb-version-output-parity.md) |
| [ADR-0031: Iterators](0031-iterators.md) | Iteration | Accepted | Implements merged internal and public iteration | [ADR-0055](0055-leveldb-block-iterator-parity.md); [ADR-0057](0057-leveldb-version-output-parity.md) |
| [ADR-0032: Write Path](0032-write-path.md) | Write path | Accepted | Original group-commit design remains; current implementation follows the parity revision | [ADR-0060](0060-leveldb-write-path-parity.md) |
| [ADR-0033: Memtable Flush](0033-memtable-flush.md) | Flush | Accepted | Level selection remains current; trusted internal paths and shutdown handling were refined | [ADR-0060](0060-leveldb-write-path-parity.md) |
| [ADR-0034: Compaction Picking](0034-compaction-picking.md) | Compaction | Accepted | Implements size, seek, and manual compaction selection | [Current architecture](../architecture.md) |
| [ADR-0035: Running Compactions](0035-running-compactions.md) | Compaction | Accepted | Implements compaction execution with later iterator-decoding changes | [ADR-0055](0055-leveldb-block-iterator-parity.md) |
| [ADR-0036: Seek Statistics](0036-seek-statistics.md) | Read-cost accounting | Accepted | Implements seek charging; later parity work changes timing and ownership | [ADR-0057](0057-leveldb-version-output-parity.md) |
| [ADR-0037: Database Engine](0037-database-engine.md) | Engine integration | Accepted | Integrates recovery, reads, writes, flush, and compaction | [ADR-0057](0057-leveldb-version-output-parity.md); [ADR-0064](0064-windows-filesystem-and-delivery.md) |
| [ADR-0038: Public RAII Database API](0038-public-raii-api.md) | Public API | Accepted | Implements the public facade and retained child lifetimes | [ADR-0057](0057-leveldb-version-output-parity.md); [API and options](../reference/api-and-options.md) |
| [ADR-0039: SSTable Block Compression](0039-sstable-block-compression.md) | Compression | Accepted | Implements Snappy and Zstd block support | [API and options](../reference/api-and-options.md) |
| [ADR-0040: Compatibility, Crash, and Fuzz Harness](0040-compatibility-and-crash-harness.md) | Verification | Accepted | Implements compatibility, crash, model, and fuzz tiers | [Building and testing](../development/building-and-testing.md) |
| [ADR-0041: Engine Hardening Gates](0041-engine-hardening-gates.md) | Verification | Accepted | Establishes sanitizer, coverage, and hardening gates | [CI and quality gates](../development/ci-and-quality-gates.md) |

## Measurement and LevelDB parity

| ADR | Topic | Lifecycle | Implementation / outcome | Supersession / current reference |
|---|---|---|---|---|
| [ADR-0042: Benchmark and Profiling Foundation](0042-benchmark-profiling-foundation.md) | Measurement | Accepted | Establishes retained benchmark and profile contracts | [Benchmarking and profiling](../development/benchmarking-and-profiling.md) |
| [ADR-0043: Profile-Guided Bytewise Comparison](0043-profile-guided-bytewise-comparison.md) | Comparator experiment | Rejected | Candidate failed admission; original comparator remains | [Benchmarking and profiling](../development/benchmarking-and-profiling.md) |
| [ADR-0044: Profile-Guided CRC32C Acceleration](0044-profile-guided-crc32c-acceleration.md) | Checksum optimization | Accepted | Candidate passed its admission gates | [ADR-0059](0059-leveldb-hardware-crc32c-control.md) |
| [ADR-0045: Fixed-Work Write and Mixed Profiling](0045-fixed-work-write-profiling.md) | Measurement | Accepted | Adds fixed-work write and mixed workload families | [Benchmarking and profiling](../development/benchmarking-and-profiling.md) |
| [ADR-0046: Invariant-Based Decoding of Validated Blocks](0046-validated-block-decoding-experiment.md) | Decoder experiment | Superseded | Historical accepted experiment; implementation later replaced | [ADR-0049](0049-leveldb-style-point-read-baseline.md); [ADR-0055](0055-leveldb-block-iterator-parity.md) |
| [ADR-0047: Inline Block-Iterator Key Reconstruction](0047-inline-iterator-key-experiment.md) | Iterator experiment | Rejected | Candidate failed admission and was not committed | [ADR-0055](0055-leveldb-block-iterator-parity.md) |
| [ADR-0048: Documentation-Only CI Routing](0048-documentation-only-ci.md) | CI routing | Accepted | Adds documentation-only change routing | [CI and quality gates](../development/ci-and-quality-gates.md) |
| [ADR-0049: LevelDB-Style Point-Read Baseline](0049-leveldb-style-point-read-baseline.md) | Read-path parity | Accepted | Establishes the first measured baseline; eager validation was later replaced | [ADR-0055](0055-leveldb-block-iterator-parity.md) |
| [ADR-0050: POSIX Mmap-Backed Table Reads](0050-posix-mmap-table-reads.md) | POSIX read I/O | Accepted | Adds POSIX mapped table reads with a copied control | [ADR-0056](0056-leveldb-table-mmap-parity.md); [platform and durability](../reference/platform-support-and-durability.md) |
| [ADR-0051: Trusted Internal-Key Comparison](0051-trusted-internal-key-comparison.md) | Comparator experiment | Rejected | Candidate failed admission and was not committed | [ADR-0055](0055-leveldb-block-iterator-parity.md) |
| [ADR-0052: Cross-Engine Point-Read Gap Decomposition](0052-cross-engine-read-gap-decomposition.md) | Read-path measurement | Superseded | Tooling plan was replaced before implementation | [ADR-0053](0053-leveldb-read-path-parity.md) |
| [ADR-0053: Pinned LevelDB Read-Path Parity Baseline](0053-leveldb-read-path-parity.md) | Read-path parity | Implemented | Completes the integrated read-path parity program | [Benchmarking and profiling](../development/benchmarking-and-profiling.md) |
| [ADR-0054: Pinned LevelDB Cache Parity](0054-leveldb-cache-parity.md) | Cache parity | Implemented | Replaces avoidable cache hot-path costs | [ADR-0053](0053-leveldb-read-path-parity.md) |
| [ADR-0055: Pinned LevelDB Block-Iterator Parity](0055-leveldb-block-iterator-parity.md) | Decoder parity | Implemented | Adopts lazy checked decoding and iterator parity | [ADR-0053](0053-leveldb-read-path-parity.md) |
| [ADR-0056: Pinned LevelDB Table and Mapped-Block Parity](0056-leveldb-table-mmap-parity.md) | Table-read parity | Implemented | Preserves mapped uncompressed block storage | [ADR-0053](0053-leveldb-read-path-parity.md) |
| [ADR-0057: Pinned LevelDB Version and Output Parity](0057-leveldb-version-output-parity.md) | Read ownership parity | Implemented | Adds lazy version visitation, read pins, and reusable output | [ADR-0053](0053-leveldb-read-path-parity.md); [API and options](../reference/api-and-options.md) |
| [ADR-0058: 4 KiB Hot-Set Fixed-Cost Alignment](0058-4k-hot-set-fixed-cost-alignment.md) | Read-path optimization | Implemented | Comparator-only candidate passed and was accepted; decoder stage was unnecessary | [ADR-0053](0053-leveldb-read-path-parity.md) |
| [ADR-0059: Pinned LevelDB Hardware CRC32C Control](0059-leveldb-hardware-crc32c-control.md) | Checksum control | Measurement-only | Quantifies hardware CRC effects without production changes | [Benchmarking and profiling](../development/benchmarking-and-profiling.md) |
| [ADR-0060: LevelDB Write-Path Parity](0060-leveldb-write-path-parity.md) | Write-path parity | Implemented | Completes matched write-path mechanisms and validation | [Benchmarking and profiling](../development/benchmarking-and-profiling.md) |
| [ADR-0061: Typed Database State Inspection](0061-typed-database-state-inspection.md) | Diagnostics | Implemented | Adds typed read-only state inspection | [Storage diagnostics](../reference/storage-diagnostics.md) |
| [ADR-0062: Read-Only Storage File Diagnostics](0062-read-only-storage-file-diagnostics.md) | Diagnostics | Implemented | Adds offline storage-file inspection | [Storage diagnostics](../reference/storage-diagnostics.md) |
| [ADR-0063: Audit Contract and Validation Repairs](0063-audit-contract-and-validation-repairs.md) | Quality repair | Implemented | Closes cross-contract validation gaps | [CI and quality gates](../development/ci-and-quality-gates.md) |

## Windows delivery and documentation governance

| ADR | Topic | Lifecycle | Implementation / outcome | Supersession / current reference |
|---|---|---|---|---|
| [ADR-0064: Windows Filesystem and Sequential Delivery](0064-windows-filesystem-and-delivery.md) | Windows roadmap | Implemented | Defines and completes the supported Windows boundary | [Platform and durability](../reference/platform-support-and-durability.md) |
| [ADR-0065: Windows Recovery and Compatibility Verification](0065-windows-recovery-and-compatibility-verification.md) | Windows verification | Implemented | Verifies native recovery, compatibility, and crash behavior | [Building and testing](../development/building-and-testing.md) |
| [ADR-0066: Native Windows Diagnostic Command](0066-native-windows-diagnostic-command.md) | Windows diagnostics | Implemented | Adds the native diagnostic executable and output contract | [Storage diagnostics](../reference/storage-diagnostics.md) |
| [ADR-0067: Windows Comparative Benchmark Baseline](0067-windows-comparative-benchmark-baseline.md) | Windows measurement | Implemented | Establishes the copied-read baseline and retained provenance | [ADR-0068](0068-windows-mapped-read-parity.md); [benchmarking and profiling](../development/benchmarking-and-profiling.md) |
| [ADR-0068: Windows Mapped-Read Reference Parity](0068-windows-mapped-read-parity.md) | Windows read I/O | Implemented | Aligns native mappings with pinned LevelDB | [Platform and durability](../reference/platform-support-and-durability.md) |
| [ADR-0069: Native Windows Selected-Workload Profiling](0069-native-windows-selected-workload-profiling.md) | Windows profiling | Implemented | Adds owned native CPU capture and validation | [Benchmarking and profiling](../development/benchmarking-and-profiling.md) |
| [ADR-0070: Documentation Information Architecture](0070-documentation-information-architecture.md) | Documentation governance | Accepted | Defines audiences, authorities, lifecycle policy, migration DAG, and quality boundaries | [Reference index](../reference/README.md); [development index](../development/README.md); [architecture](../architecture.md) |
| [ADR-0071: Four-Space C++ Formatting](0071-four-space-cpp-formatting.md) | Code style | Accepted | Establishes four-space formatting informed by Catch2 and a mechanical source migration | [Code style](../development/code-style.md) |

## Historical upstream-link provenance

Twenty-five ADRs originally contained 33 mutable upstream links without
recording the revision reviewed at decision time. On 2026-10-09, maintenance
passes pinned those links to the then-verified branch revisions:

- RocksDB `main`: `928527b86951a91367415b0023306735e8f0961b`;
- Pebble `master`: `e0818dd07ec580eb229e8291b4eefdb135cd5a55`;
- YCSB `master`: `66302f301b13f60d4bcb2f29f478586bb1d6f2e0`.

This makes the retained links immutable; it does not claim that these
revisions were the original review baselines.
