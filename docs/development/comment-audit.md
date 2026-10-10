# Source Comment Audit

[Development guide](README.md) | [Remediation plan](comment-remediation-plan.md)

- Audit date: 2026-10-10.
- Source baseline: `c80d7753a22ba39df12fa752e2691cee916ae02d`.
- Scope: repository-owned code, tests, benchmarks, fuzzers, tools, and build/CI scripts.
- Delivery: fixed-baseline assessment; local implementation is tracked in the remediation plan.
- Language: retain English.

This report describes the pre-remediation source, including its original line
numbers. The [implementation follow-up](comment-remediation-plan.md#execution-review)
uses this engine's concepts rather than upstream alignment as the teaching
objective; it supersedes upstream-centric wording in the recommendations below.

## Assessment

The repository has useful **maintainer-oriented contracts**, especially in
the engine and metadata headers, but not yet a consistent **source-local
learning experience**. A reader entering through the public API, memory
structures, or format headers must reconstruct important concepts and
lifetime rules from implementations, tests, or separate lessons.

The main remedy is not more comments everywhere. It is to put a small,
accurate explanation at each important boundary, explain the invariant at
non-obvious transitions, and connect that explanation to executable evidence.
Existing learning lessons should remain the place for extended examples.

Three concrete comment defects were found: an iterator contract attached to
the sampling type, an overly restrictive explanation of exception sources,
and an ownership statement that does not cover borrowed filter contents.
Other findings below are missing contracts, learning gaps, or maintenance
risks, not demonstrated product bugs.

## Scope and method

The inventory starts with Git-tracked files, not a recursive scan of the
working directory. It includes `.cc`, `.h`, `.cmake`, `.py`, `.yml`, and
`CMakeLists.txt`. Generated build files, fetched dependencies, bytecode,
binary fixtures, Markdown prose, and license text are not counted as code
comments. Existing documentation was read as supporting context.

C++ comments were extracted while skipping quoted literals and raw strings.
Python comments were tokenized, and actual module/class/function docstrings
were identified through the syntax tree. Build/CI script counts cover
full-line `#` comments; comment-like text embedded in strings is not treated
as explanatory prose.

All extracted explanatory/other comment text and Python docstrings were
read. Namespace/include-guard annotations and coverage exclusions were
reviewed separately. The findings were checked against the relevant
declarations, implementations, tests, and current learning/reference pages.
This is a repository-wide **comment-text review with targeted semantic
verification**, not a claim that all 55,895 source lines received a complete
behavioral or concurrency audit.

### Inventory

An item is one C++ line/block comment token, one Python comment/docstring,
or one full-line script comment. Adjacent `//` lines count separately; a
multi-line block comment or docstring counts once.

| Area | Files | Source lines | Explanatory/other items | Mechanical items | Coverage items | Files with no explanatory/other items |
|---|---:|---:|---:|---:|---:|---:|
| Public headers | 12 | 554 | 56 | 24 | 0 | 4 |
| Production source | 98 | 15,854 | 634 | 188 | 155 | 35 |
| Tests and test support | 110 | 29,531 | 439 | 168 | 0 | 70 |
| Benchmarks | 7 | 3,686 | 2 | 9 | 0 | 6 |
| Fuzzers | 4 | 401 | 0 | 6 | 0 | 4 |
| Tools | 9 | 4,477 | 9 | 6 | 0 | 2 |
| CMake modules | 9 | 630 | 9 | 0 | 0 | 6 |
| Root CMake file | 1 | 208 | 1 | 0 | 0 | 0 |
| CI workflows | 2 | 554 | 0 | 0 | 0 | 2 |
| **Total** | **252** | **55,895** | **1,150** | **401** | **155** | **129** |

The inventory contains 206 C++ files, 19 Python files, 14 CMake modules,
11 `CMakeLists.txt` files, and two workflows. Thirteen of the items are
Python docstrings.

"Explanatory/other" excludes namespace/include-guard labels, shebangs, and
coverage markers, but still includes formatter/linter directives and an
unused-parameter annotation. For example, the benchmark area's two items
are `clang-format` directives, not teaching comments. These counts are
navigation aids, **not a documentation coverage score**. A short wrapper
or a clearly named test may correctly need no additional comment.

### Detailed production coverage

| Source area | Files | Explanatory/other items | Review disposition |
|---|---:|---:|---|
| Public base headers | 6 | 21 | Preserve comparator/checksum contracts; connect encoding and error types to their lessons |
| Public handles/options/state | 6 | 35 | Fill handle and operation contracts; preserve existing options/state warnings |
| API facade | 5 | 3 | Preserve destruction-order explanations; document public behavior in public headers |
| Base implementation | 4 | 1 | Keep simple wrappers terse; explain compatibility-specific decoding choices |
| Cache | 3 | 0 | Add ownership, pin, capacity, and eviction invariants |
| Diagnostics | 8 | 5 | Explain offline validation boundaries without narrating output formatting |
| Engine | 25 | 406 | Correct misplaced documentation; split dense policy descriptions and explain transitions |
| Formats | 6 | 2 | Add compact format maps, validation boundaries, and borrowed-view lifetimes |
| Instrumentation | 2 | 7 | Preserve epoch lifetimes; clarify measurement interpretation |
| Memory | 5 | 4 | Add single-writer/publication and arena-lifetime contracts |
| Metadata | 8 | 63 | Preserve version/MANIFEST contracts; connect installation to durability reasoning |
| Platform | 14 | 12 | Put filesystem and executor obligations at their interfaces |
| Table | 16 | 126 | Correct ownership/exception prose; explain restart/index boundaries |
| WAL stream I/O | 2 | 5 | Add result taxonomy, borrowed-record lifetime, and writer state contracts |

Test coverage includes all 18 support files, 12 Python test/smoke files,
61 unit C++ files, six compatibility/crash/model C++ files, and 13 build
fixture/configuration files. All seven benchmark files, four fuzzer/build
files, nine tool/build files, nine standalone CMake modules, the root build
file, and both workflows are included.

## What already works

- [Comparator contracts](../../include/modern_leveldb/base/comparator.h),
  lines 12-39, describe semantic identity, concurrency, separator bounds,
  and static lifetime rather than restating signatures.
- [Database-state comments](../../include/modern_leveldb/database_state.h),
  lines 22-45, distinguish an owning observation from exact process-memory
  accounting and a complete error history.
- [Options warnings](../../include/modern_leveldb/options.h), lines 30-50,
  explain mmap faults, Bloom/comparator compatibility, and weak Windows
  namespace durability. These warnings should not be diluted.
- [Facade ownership](../../src/api/api_internal.h), lines 40-42 and 76-80,
  makes reverse destruction order a visible lifetime argument.
- [Version-set contracts](../../src/metadata/version_set.h), lines 104-115,
  describe validation, sticky I/O failure, and lock restoration.
- [Block validation](../../src/table/block.h), lines 22-23 and 52-54,
  correctly separates restart-array validation from lazy entry validation.
- Engine tests often explain causality well. For example,
  [compaction tests](../../tests/unit/engine/compaction_test.cc), lines
  427-455, explain threshold equality and why even dropped entries affect
  grandparent-overlap accounting.

These are patterns to extend, not material to replace wholesale.

## Findings

Priorities concern comment remediation, not vulnerability severity:

- **P1:** misleading statements or missing safety/lifetime contracts.
- **P2:** important teaching, discoverability, or maintainability gaps.

| ID | Priority | Kind | Main location | Finding |
|---|---|---|---|---|
| C01 | P1 | Incorrect attachment | `src\engine\db_iterator.h:18-35` | The iterator contract is placed above `ReadSampling`, not `DbIterator` |
| C02 | P1 | Overstated contract | `src\table\table_builder.h:35-39` | Exceptions are described as allocation-only despite extensible callees |
| C03 | P1 | Incorrect ownership | `src\table\filter_block.h:54-60` | The reader is described as owning bytes even when contents are borrowed |
| C04 | P1 | Missing public contracts | `include\modern_leveldb\iterator.h:13-29`; `db.h:24-49` | Important lifetime, positioning, absence, and concurrency rules are not source-local |
| C05 | P1 | Missing concurrency contract | `src\memory\skiplist.h:22-66, 124-174` | Atomic links have no adjacent single-writer/publication explanation |
| C06 | P1 | Missing interface contracts | `src\platform\file_system.h:25-104`; `background_executor.h:18-40` | I/O, durability, scheduling, and shutdown obligations live elsewhere |
| C07 | P2 | Missing format/validation map | `src\format\internal_key.h:18-40`; `write_batch.h:15-40` | Readers cannot see the representation and checked/trusted boundary at entry |
| C08 | P1 | Missing borrowed-result contract | `src\wal\wal_io.h:36-71` | The result taxonomy and returned-record lifetime are undocumented |
| C09 | P1 | Missing cache ownership contract | `src\cache\sharded_lru_cache_core.h:16-55` | Pins, transferred values, soft capacity, and reclamation lack local explanation |
| C10 | P2 | Difficult algorithm exposition | `src\engine\compaction_picker.h:48-66`; `flush.h:32-40` | Dense prose and upstream names require substantial prior knowledge |
| C11 | P2 | Missing evidence interpretation | `tests\unit\format\internal_key_test.cc:193-235`; `fuzz\format_fuzzer.cc:26-40` | Some byte vectors, model boundaries, and fuzz budgets lack a learning explanation |
| C12 | P2 | Missing measurement/diagnostic map | `benchmarks\profiling_bench.cc`; `tools\run_performance.py`; `src\diagnostics\dump_file.h:17-18` | Complex evidence machinery lacks source-local scope and interpretation |
| C13 | P2 | Incomplete review guidance | `docs\development\code-style.md`, API and comments section | The standard says what to avoid but not how to build a consistent learning layer |

### C01: Put the iterator contract on the iterator

The first six lines of the leading comment describe iteration, `key()`,
`value()`, and movement failure. The following lines describe sampling.
All of that text currently precedes `struct ReadSampling`; `class
DbIterator` begins later without its own type-level explanation.

The text is largely useful, but its placement obscures which type owns
which contract. Separate the sampling explanation from the iterator
contract, preserving the forward/backward invariant at lines 72-75.
Evidence: [header](../../src/engine/db_iterator.h) and
[iterator tests](../../tests/unit/engine/db_iterator_test.cc).

### C02: State the exception recovery rule, not an unsupported cause restriction

The `TableBuilder` comment says that only allocation failure causes a
throw. The implementation calls virtual writable-file operations and
custom comparator shortening methods without enforcing that restriction:
[table builder](../../src/table/table_builder.cc), lines 82-84, 165-173,
and 189-190; [filesystem interface](../../src/platform/file_system.h),
lines 56-59; [comparator interface](../../include/modern_leveldb/base/comparator.h),
lines 32-35.

Injected files can throw through the documented
[test filesystem hook](../../tests/support/memory_file_system.h), lines
43-46. Therefore the allocation-only sentence is not a complete contract,
even though normal I/O failures use `Status`.

Retain the no-reuse-after-exception rule and describe callback/backend
exceptions without promising a narrower set of causes. Review the similar
wording in [filter_block.h](../../src/table/filter_block.h), lines 15-17,
separately; this finding does not claim that its concrete policy has the
same virtual-file call path.

### C03: Owning the wrapper does not always mean owning its bytes

`FilterBlockReader` is described as reading a validated block "that it
owns." That is true for the vector overload, but the `BlockContents`
overload preserves borrowed storage. Its constructor moves the wrapper
without materializing an owning copy:
[filter_block.cc](../../src/table/filter_block.cc), lines 101-121 and
136-141.

[BlockContents](../../src/table/block_format.h), lines 38-58, explicitly
supports either owned or borrowed bytes. The table filter-loading path
passes those contents directly to the reader:
[table.cc](../../src/table/table.cc), lines 167-176.

Document both forms and require the backing storage to remain alive and
unchanged for borrowed contents. Do not change the mmap path to copy
merely to make the old comment true.

### C04: Public readers should not need internal headers to use handles safely

The public iterator, snapshot, and batch headers contain no explanatory
comments. `db.h` documents batch copying/exclusive access and state
observation, but not most database/read/child-handle contracts.

Important behavior is already present in
[the API reference](../reference/api-and-options.md),
[ownership lesson](../learning/08-cpp-ownership-errors-and-concurrency.md),
and implementations:

- An iterator begins unpositioned; accessors and `Next`/`Prev` require a
  valid position. Key/value views expire when it moves or is destroyed.
- Public child handles retain the engine; the internal iterator's
  must-outlive-engine rule must not be copied unchanged into the facade.
- A snapshot pins visibility, not a database copy. A read must use a live
  snapshot from the same database.
- The reusable `Get` returns `false` for absence/deletion and leaves the
  output unchanged in that case; the owning overload uses an empty optional.
- Concurrent database operations do not permit racing handle movement or
  destruction. A single iterator/batch is not automatically thread-safe.
- A write failure is not proof that its WAL bytes cannot appear in recovery.

Evidence: [public facade](../../src/api/database.cc), lines 50-80 and
210-270; [iterator facade](../../src/api/iterator.cc), lines 26-35;
[public API tests](../../tests/unit/api/public_api_native_test.cc), lines
136-156, 218-242, 325-430.

Place short contracts at the public declarations. Keep extended usage and
platform guarantees in the current reference pages.

### C05: Explain what the skip-list atomics do and do not protect

The skip list has no explanatory comments, although it combines acquire/
release links, relaxed height updates, a mutable PRNG, arena allocation,
and a compact variable-height node layout.

The missing contract should identify one externally serialized writer,
concurrent traversal of published nodes with immutable keys, borrowed comparator/
arena lifetimes, and the unique-key precondition of `InsertTrusted`.
It should explain why initializing the new link with relaxed ordering is
different from publishing the node through the predecessor's release store,
and why a relaxed height hint does not expose an uninitialized node.

Evidence: [skiplist.h](../../src/memory/skiplist.h), lines 124-174 and
263-267; [concurrent-reader test](../../tests/unit/memory/skiplist_test.cc),
lines 262-343; [publication lesson](../learning/08-cpp-ownership-errors-and-concurrency.md).
The lesson explains the concept, but the source-local proof is absent.
Do not describe the entire structure or engine as multi-writer or lock-free.

Also document arena-wide reclamation and distinguish
`MemTable::memory_usage()` from safely published engine accounting.

### C06: Put backend obligations where backend implementers look

`file_system.h` comments only stable borrowed reads and default-backend
creation. Core methods do not explain short reads/EOF, output size,
append/open semantics, or the difference between `Flush`, `Sync`, `Close`,
and `SyncDirectory`.

`background_executor.h` does not explain scheduling and teardown. The
engine's injected-executor requirement is documented instead in
[database.h](../../src/engine/database.h), lines 61-64.
[SerialExecutor](../../src/platform/serial_executor.cc), lines 17-29,
stops/joins its worker and discards queued tasks during teardown; do not
invent a universal drain-on-destruction guarantee.

Add interface contracts consistent with the native backends and
[durability lesson](../learning/04-wal-and-recovery.md). Keep the general
executor behavior separate from the stronger engine-caller obligation
that accepted work eventually runs while the engine is open. Preserve
the weaker native Windows namespace boundary.

### C07: Give format readers a compact representation and validity map

The internal-key header has one explanatory line; WAL framing has none;
the batch header only explains the trusted-open precondition.

Add a small layout explanation for each representation, its ordering or
framing purpose, and the invalidation rule for borrowed views. Explain
that checked batch opening validates all records before `Next`, whereas
trusted opening relies on the owned batch's invariant and unchanged storage:
[write_batch.cc](../../src/format/write_batch.cc), lines 100-174.

For internal keys, explain user-key ascending/trailer descending comparison,
not a lexicographic comparison of the encoded bytes. For the WAL cursor,
document its borrow of both the fragmenter state and unchanged payload.
Reuse [lesson 02](../learning/02-bytes-and-formats.md) and
[lesson 04](../learning/04-wal-and-recovery.md) instead of duplicating
their complete examples.

### C08: Explain WAL events, terminal failures, and view invalidation

`Result<optional<variant<...>>>` communicates types, but not policy:
record, recoverable corruption event, EOF, and terminal read failure are
different outcomes. A record's `ByteView` refers to the reader's block or
scratch storage; callers should consume/copy it before the next read and
before reader destruction.

Also state that `AddRecord` flushes but does not imply `Sync`, and that
ordinary writer I/O failure poisons later writes while close still releases
the file. Explain the reopen-padding policy at the constructor.

Evidence: [wal_io.cc](../../src/wal/wal_io.cc), lines 31-105 and 136-209;
[WAL tests](../../tests/unit/wal/wal_io_test.cc), lines 222-423 and
425-733. Keep the caller's WAL-versus-MANIFEST corruption policy separate
from the reader's event contract.

### C09: Make cache retention and reclamation teachable

The typed and erased cache headers/implementation contain coverage
annotations but no explanatory comments.

Document value-ownership transfer, the cache-outlives-pin obligation,
replacement/erase preserving outstanding pins, per-shard soft capacity,
zero-capacity/charge-overflow uncached pins, and the distinction between
`lru` and `in_use`. Explain why retired value destruction happens outside
the shard mutex, rather than adding a comment for every list operation.

Evidence: [cache implementation](../../src/cache/sharded_lru_cache_core.cc),
lines 195-245 and 310-370;
[cache tests](../../tests/unit/cache/sharded_lru_cache_test.cc), lines
127-148, 223-308, and 382-402. The table-cache header already offers a
useful model of a capacity target rather than a hard open-file bound.

### C10: Separate policy, prerequisites, and the reason for each transition

Compaction picking and flush placement have substantial comments, but
several paragraphs interleave policy, numerical limits, preconditions,
and upstream function names. A novice must parse the entire paragraph
before learning the main rule.

Keep upstream references as provenance, but first explain the local idea:
why level 0 expands transitively, why boundary inputs preserve user-key
history, why snapshots retain old versions, and why tombstones cannot be
dropped above an older value.

Add brief invariant comments at block seek/backward reconstruction,
write-sequence publication, and metadata installation where needed.
For example, [block.cc](../../src/table/block.cc), lines 230-310, contains
binary-search/restart logic but no ordinary explanatory comments.
The current block-header validation contract should remain intact.

Split long declaration comments into summary, conditions, and error/
lifetime paragraphs. Avoid duplicating the same full algorithm in a header,
implementation, lesson, and ADR.

### C11: Explain why evidence catches the failure, not just what the test does

Behavioral test names are generally good; a comment on every test would
make them worse. The gap is concentrated in opaque evidence:

- Internal-key golden vectors show bytes but not their field decomposition
  or the derivation/provenance of the compatibility oracle.
- The block golden helper identifies restart offsets, but does not show
  the small size calculation that produces offset 18.
- The crash harness's acceptable recovered states deserve a local
  explanation of acknowledged versus in-flight updates.
- The format fuzzer's decode budget and database fuzzer's bounded command
  stream do not describe what they constrain or leave untested.

Evidence: [golden keys](../../tests/unit/format/internal_key_test.cc),
lines 193-235; [golden block](../../tests/unit/table/block_test.cc), lines
116-130; [crash state matching](../../tests/crash/power_loss_test.cc),
lines 136-220; [format fuzzer](../../fuzz/format_fuzzer.cc), lines 26-40;
[database fuzzer](../../fuzz/database_fuzzer.cc), lines 22-65.

Annotate representative vectors and model/budget boundaries only.
Distinguish compatibility evidence from self-consistent round trips, and
crash simulation from proof of arbitrary native power-loss behavior.

### C12: Explain measurement and diagnostic boundaries at entry points

The 1,831-line selected-workload benchmark has no explanatory comments.
The major performance/parity scripts have only module docstrings. Their
names and validators help, but do not give readers a local map of setup,
warmup, fixed work, measured epochs, result ownership, provenance, and
cleanup.

Add entry-point maps and comments at the subtle boundaries, not a second
schema specification in prose. Explain that inclusive diagnostic stages
are not additive exclusive costs and that instrumentation timing is not
ordinary throughput evidence. The Windows epoch protocol should explain
the purpose of its before/after observation.

The offline dump declaration warns about output aliasing but omits the
closed-database requirement and limited validation scope already documented
in [storage diagnostics](../reference/storage-diagnostics.md).

Use [benchmarking and profiling](benchmarking-and-profiling.md) as the
current procedural reference. Do not copy historical parity conclusions
into permanent source comments as timeless performance guarantees.

### C13: Define the teaching layer without introducing comment quotas

The style guide correctly requires English, borrowed-view contracts, and
non-obvious invariants while rejecting line-by-line narration. It does not
yet define a common entry-point shape or a source-to-lesson/test navigation
rule.

Extend that guide with an optional short module/type summary, operation
contracts at declarations, causal invariants at transitions, and evidence
links where readers otherwise cannot find the relevant example. No
mandatory comment on every function, density threshold, generator
migration, or custom lint framework is justified by this audit.

The existing documentation checker validates Markdown structure and
selected machine-referenced contracts, not source-comment semantics:
[check_documentation.py](../../tools/check_documentation.py), lines 992-1017.
Passing that checker cannot establish that a concurrency explanation is true.

## Annotation handling

The 401 mechanical items and 155 coverage items are not learning prose.
Coverage annotations generally include a local reason, such as an
unreachable validated enum, large allocation threshold, compiler clone,
or unavailable syscall seam. Preserve their machine-recognized spelling
and their justifications.

Do not delete or reword `GCOVR_EXCL_*`, `NOLINT`, or `clang-format`
directives as cosmetic cleanup. If an exclusion's premise changes,
recheck it against code and the coverage policy. This audit does not
certify the exclusion decisions or a fresh coverage result.

## Target reading experience

A reader should be able to enter a core header and answer:

1. What problem does this type solve in the engine?
2. What must be valid before calling it?
3. Who owns each returned object/view, and what invalidates it?
4. Which synchronization or persistence boundary matters?
5. Which implementation and test demonstrate the non-obvious rule?

Keep the layers complementary: **lesson for concepts and worked examples;
header for caller obligations; implementation for invariant transitions;
test for executable evidence; ADR for decision history**.

The [remediation plan](comment-remediation-plan.md) turns findings C01-C13
into bounded changes with dependencies and acceptance criteria.
