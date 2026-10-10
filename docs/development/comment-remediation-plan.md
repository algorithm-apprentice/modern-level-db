# Source Comment Remediation Plan

[Development guide](README.md) | [Audit report](comment-audit.md)

- Plan date: 2026-10-10.
- Based on audit baseline: `c80d7753a22ba39df12fa752e2691cee916ae02d`.
- Status: implemented, locally validated, and independently reviewed; PR checks govern merge.
- Scope: English comments and directly related documentation only.

## Goal and boundaries

Make the source readable as a connected learning document without turning
each implementation into a textbook or duplicating the existing
[learning path](../learning/README.md).

Organize explanations around this engine's concepts: representation,
visibility, persistence, ownership, concurrency, and reclamation. Explain
the local rule and why it works before naming another implementation.
Upstream alignment is not a teaching objective. Retain provenance only
where it establishes a persistent-format contract or an independent test
oracle; preserve the actual compatibility behavior and identities.

The primary audience knows basic C++ and tests but is learning storage
engines. Deeper ownership, format, and concurrency notes should remain
useful to maintainers.

No public API, persistent format, synchronization, error-handling behavior,
allocation policy, benchmark workload, or coverage exclusion is to change
in a comment-remediation batch. If checking a claim reveals a product bug
or an unestablished guarantee, record it separately instead of silently
repairing code or inventing the guarantee in a comment.

Do not add Doxygen infrastructure, documentation generators, comment
density gates, or comments for obvious accessors. Keep ordinary `//`
comments unless the project separately approves a different convention.

## Comment placement standard

Extend [code style](code-style.md) with these rules in R0:

| Location | Include when needed | Avoid |
|---|---|---|
| Module/type entry | Role, representation, important ownership/synchronization model, next reading point | Full class-member catalog |
| Public/internal declaration | Preconditions, result/absence meaning, borrowing/invalidation, concurrency, failure/exception state | Restating the signature |
| Non-obvious implementation transition | Why the next state remains valid; publication, retention, or durability boundary | Narrating assignments and loops |
| Representative test/fixture | Why the vector, threshold, schedule, or allowed outcome catches a failure | Repeating a descriptive test name |
| Lesson/reference | Worked examples, diagrams, usage, broader explanation | Duplicating every implementation detail |
| ADR | Decision, alternatives, history, supersession | Serving as the only current caller contract |

Use summary-first prose. Add separate paragraphs for prerequisites and
failure/lifetime rules when a contract becomes dense. Name the protecting
mutex, retaining owner, or invalidation event instead of writing only
"thread-safe" or "valid."

Source comments may use repository-relative lesson/test paths and stable
symbol or test names as navigation. Do not put line numbers in permanent
source comments; the audit's line references identify its fixed baseline.
Avoid repeating literal defaults or thresholds unless the number itself
explains a format or compatibility decision.

### Placement examples

The intended skip-list entry contract is short:

```cpp
// Arena-backed ordered index with one externally serialized writer and
// concurrent readers. Published keys are immutable; nodes are reclaimed only
// with the arena. The comparator and arena must outlive the list and its readers.
```

The publication comment belongs beside the predecessor link store, not
beside every atomic operation:

```cpp
// Initialize the new node's link before release-publishing it through the
// predecessor. Readers acquire that link before inspecting the node.
```

An iterator contract should expose the borrow boundary:

```cpp
// Initially unpositioned. key(), value(), Next(), and Prev() require valid().
// Key/value views expire when the iterator moves or is destroyed.
```

These illustrate placement rather than reproduce exact source text. Check
the type, move behavior, caller, and lifetime before applying the pattern.

## Work batches and dependencies

Deliver each batch as a coherent, sequential change under the repository's
existing workflow. The dependencies below describe actual evidence reuse,
not permission to launch parallel workflows.

| Batch | Findings | Scope | Prerequisites | Required outcome |
|---|---|---|---|---|
| R0: Correct claims and establish style | C01, C02, C03, C13 | Iterator sampling attachment, table/filter contracts, comment guidance | None | Correct misleading statements and agree on the lightweight teaching pattern |
| R1: Public API learning surface | C04 | Public handles, operations, options, bytes/errors | R0 | Readers can use the facade safely from its declarations |
| R2: Bytes, memory, and publication | C05, C07 | Base codecs, internal keys, batches, arena, skip list, memtable | R0 | Representation and validation/lifetime/publication boundaries are explicit |
| R3: I/O, WAL, and recovery | C06, C08 | Filesystem/executor interfaces, WAL streams, recovery, version installation | R0, R2 | EOF/corruption/I/O and file/namespace/visibility boundaries are distinguishable |
| R4: Tables, caches, and read paths | C09, C10 | Blocks, filters, tables, caches, point reads, iterators | R0, R2, R3 | Index/restart/pin rules explain read correctness and retention |
| R5: Writes, flush, and compaction | C10 | Queue, commit, backpressure, flush placement, compaction picking/running | R2, R3, R4 | Four core maintenance/write transitions have causal explanations |
| R6: Evidence machinery and reading pass | C11, C12, C13 | Targeted tests, fuzzers, benchmarks, diagnostic/profiling tools, lesson navigation | R1, R2, R3, R4, R5 | The learning route connects explanations to evidence without overstating its scope |

R0-R3 address misleading or missing safety contracts before broader
algorithm exposition. R4-R6 finish the source-local learning experience.
No deadline or effort estimate is asserted without reviewing the final
diff scope of each batch.

### Execution review

The scope, dependencies, and acceptance criteria are suitable for implementation.
Local batches may be applied and validated sequentially in one working tree;
this authorization does not create commits, pull requests, or remote changes.
The existing independent-review and merge gates still apply to delivery.

Two qualifications prevent the plan from overstating its verification:
non-comment C++ token equality excludes source-location shifts caused by added
lines, and a build/test pass does not prove a comment's concurrency or durability
claim. Those claims must be checked against the implementation and its callers.
Python additions use comments rather than new executable statements or docstrings.

| Batch | Local implementation | Delivery review/merge |
|---|---|---|
| R0 | Implemented | Reviewed; PR checks/merge pending |
| R1 | Implemented | Reviewed; PR checks/merge pending |
| R2 | Implemented | Reviewed; PR checks/merge pending |
| R3 | Implemented | Reviewed; PR checks/merge pending |
| R4 | Implemented | Reviewed; PR checks/merge pending |
| R5 | Implemented | Reviewed; PR checks/merge pending |
| R6 | Implemented | Reviewed; PR checks/merge pending |

The local changes cover all three concrete defects and the targeted learning
gaps C04-C13. Explanations follow representation, visibility, persistence,
ownership, concurrency, and reclamation; another implementation's function
name is no longer used as the explanation of a production rule. Independent
fixture origins and persistent identities remain intact.

The existing source tours and guided labs keep their entry points: this work
adds contracts and reasoning around the same symbols, not a second lesson
series. Every audited file has a disposition below. The
[independent review](#independent-delivery-review) and
[local validation record](#local-validation-record) cover pre-delivery checks;
the PR must still pass CI on the exact reviewed head before merging.

## R0: Correct claims and establish style

Change the smallest set first:

- Move the iteration/lifetime contract to `DbIterator`; leave only sampling
  policy above `ReadSampling`.
- Remove `TableBuilder`'s allocation-only restriction while preserving its
  no-reuse-after-exception rule. Check analogous builder wording separately.
- Describe owned and borrowed `FilterBlockReader` contents and the backing
  storage obligation.
- Add the placement/evidence rules above to the existing style guide.

**Acceptance:** all three concrete defects have corrected text at the
appropriate declaration; owned and borrowed paths are both covered;
no formatter/linter/coverage directive or runtime token has changed.
Supporting evidence includes the table-builder error tests, mapped/copied
table paths, iterator tests, and the inspected virtual callback contracts.

## R1: Public API learning surface

Start with `db.h`, `iterator.h`, `snapshot.h`, and `write_batch.h`.
Then cover the semantic gaps in `options.h`, `bytes.h`, and `result.h`
without adding prose to every default or accessor.

Document:

- Owning handles versus non-owning views, move-only state, and child
  retention of the engine/directory lock.
- Initial iterator positioning, seek lower-bound meaning, access/movement
  preconditions, view invalidation, and per-handle concurrency.
- Snapshot visibility, same-database validation, and iterator retention
  of a supplied snapshot registration.
- Reusable/owning `Get` absence semantics; batch atomicity and copying
  versus exclusive borrowing.
- Database-operation concurrency versus forbidden move/destruction races.
- WAL synchronization, uncertainty after failure, and links to the
  platform durability boundary rather than a stronger blanket promise.

**Acceptance:** a reader using only public declarations can explain the
listed rules. Each rule maps to an implementation and existing evidence,
including `ReusesCallerOutputAndLeavesItUnchangedWhenAbsent`,
`IteratorRetainsItsSnapshotAfterTheSnapshotHandleIsDestroyed`,
`RejectsForeignAndMovedFromHandles`, and `ChildHandlesKeepTheEngineAlive`.
Do not accidentally apply an internal engine-outlives-iterator rule to
the owning public facade.

## R2: Bytes, memory, and publication

Add concise entry maps to the format and memory headers, then comment
only the implementation transitions that require a proof.

Cover internal-key packing and comparator order; batch fields and
complete checked validation; `OpenTrusted` and internal comparison
caller/lifetime obligations; arena allocation and whole-arena reclamation;
skip-list single-writer assumptions and acquire/release publication;
memtable value/deletion/missing semantics and borrowed values.

Identify the role of the relaxed height hint, writer-private PRNG, and
compact node/link layout. Distinguish a memtable's live arena accounting
from the engine's published observation used during concurrent commits.

**Acceptance:** each trusted entry point names the established invariant
and its actual caller or owner. A reader can predict equal-user-key
ordering and snapshot lookup and can explain why atomics do not permit
concurrent insertions by arbitrary writers.

Evidence includes the golden internal-key/batch tests, checked/trusted
equivalence tests, `ConcurrentReadersObserveOnlyInitializedOrderedNodes`,
`ConcurrentReadersObservePublishedEntries`, and
`ReportsArenaReservedMemoryGrowth`.

## R3: I/O, WAL, and recovery

Document short reads and EOF at the filesystem interface; append/flush/
sync/close and namespace durability; stable read-view lifetime; injected
executor scheduling obligations; serial-executor cancellation/join behavior.

Add the WAL header layout and cursor borrow/serialization requirements.
At the stream API, describe the four reader outcomes, record-view
invalidation, writer poisoning, explicit close, and reopen padding.
Explain the different WAL/MANIFEST corruption policies at their callers.

Annotate the output-file, directory, MANIFEST, `CURRENT`, and in-memory
publication transitions in recovery/version installation. Preserve the
distinction between strict POSIX namespace persistence and explicit weak
Windows consent. A backend-neutral comment must not promise a stronger
native Windows guarantee.

**Acceptance:** readers can distinguish `Flush` from `Sync`, data durability
from name durability, benign truncated tails from corruption events,
and corruption events from terminal I/O failures. A returned WAL record
cannot be mistaken for independently owned storage.

Evidence includes `FirstIoErrorPoisonsWriterButCloseStillRuns`,
`TruncatedTailAndIncompleteFragmentAreBenignEof`, `ReadErrorsAreTerminal`,
filesystem contract tests, executor teardown tests, recovery tests, and
MANIFEST installation/failure tests.

## R4: Tables, caches, and read paths

Explain prefix compression, restart checkpoints, index separators, lazy
entry validation versus table-open validation, borrowed mapped contents,
and compression/filter decisions at their relevant boundaries.

Add cache contracts for value transfer, pins, replacement, uncached
handles, soft per-shard capacity, list membership, and deletion outside
the shard lock. Keep the table-cache capacity explanation consistent.

Connect those primitives to point-read source order, level-0 newest-first
selection, disjoint deeper levels, merged iterators, and forward/backward
direction changes. Split long contracts rather than growing them.

**Acceptance:** readers can explain why a pin survives erase, why capacity
does not bound every live entry, how a seek reconstructs a prefix-compressed
key, and when persistent bytes are actually checked.

Evidence includes cache replacement/overflow/concurrency tests,
`DestroysEvictedValuesOutsideShardLock`, block iterator recovery tests,
mapped/copied table tests, lookup ordering tests, and iterator
direction-change tests.

## R5: Writes, flush, and compaction

Use the existing precise contracts as the starting point. Add causal
explanations for group admission, leader ownership while the mutex is
released, WAL-before-memtable order, final-sequence publication, first-error
containment, and write backpressure.

Split flush/compaction policy descriptions into summary, conditions,
ownership, and failure behavior. Explain transitive level-0 expansion,
boundary inputs, trivial moves, oldest-snapshot retention, tombstone
drops, pending-output protection, and installation-before-cleanup.

Use an existing recurring key/history example where it helps; do not
add a separate example beside every branch.

**Acceptance:** a reader can trace write, flush, and compaction without
consulting the upstream implementation, and can explain why an old value
or tombstone is retained. Comments distinguish committed visibility from
durability and do not describe an I/O error as certain rollback.

Evidence includes group-sync/admission tests, exception/wakeup tests,
snapshot-retention compaction tests, boundary/overlap picker tests,
directory/MANIFEST ordering tests, and obsolete-file protection tests.

## R6: Evidence machinery and reading pass

Annotate selected golden vectors with their fields and independent
provenance or derivation. Explain restart-offset arithmetic once.
Document crash-harness allowed outcomes, model assumptions, fuzzer
command/budget scope, and the limits of simulation.

Give benchmark/profiling/diagnostic entry points a short execution map:
preparation, warmup, measured/fixed work, validation, artifacts, cleanup.
Explain ownership/access/durability controls and the non-additive nature
of inclusive diagnostic stages. Document the Windows epoch observation
purpose without treating `volatile` alone as synchronization.

Keep executable validators authoritative for exact schemas. Give the
offline dump declaration its closed-database and limited-certification
boundary, consistent with the reference.

Inspect build and CI comments for non-obvious dependency/reference/gate
decisions. Existing concise CMake explanations may need no changes;
self-descriptive commands and simple wrappers should remain uncommented.

Finish by following the source tours from lessons 01-09 and the labs in
lesson 10. Correct stale pointers only where the comment work changes
the recommended reading route; do not rewrite historical ADRs wholesale.

**Acceptance:** representative evidence explains why it supports a claim
and what it does not prove. All files in the audit's 252-file scope have
an explicit final disposition in the implementation review:
changed, retained with rationale, or no explanatory comment needed.

## Validation and closure

For each batch:

1. Rebase the source references mentally and inspect current code; the
   audit's line numbers refer to its fixed baseline.
2. Compare each new claim with its implementation, actual caller, and
   named evidence. Unsupported claims block that comment.
3. Review the diff for comment/documentation-only changes. For C++ edits,
   compare non-comment token streams as well as the diff; check that
   directives and continuation lines were not altered.
4. Use the existing formatter on changed C++ files, inspecting any
   formatting-only token movement. Do not reformat unrelated files.
5. Run the existing documentation checker and its affected tests for
   Markdown changes. If C++ comments changed, run the smallest existing
   relevant build/test target; expand only for token changes or an
   affected contract requiring additional evidence.
6. Obtain independent review of correctness and learning clarity before
   considering the batch closed.

No new test is required merely to assert that a comment exists. A new
behavioral example/test is justified only when an important claimed rule
has no executable evidence, and should be separated from a pure prose
batch when it changes the validation surface.

### Final learning checks

| Reading route | Reader must explain without guessing |
|---|---|
| Public API to write queue to WAL to memtable | Atomic visibility, group sync, failure uncertainty, sequence publication |
| Get/iterator to memory/version/table/block | Source selection, snapshots, checked bytes, borrowed views, pin lifetimes |
| Recovery to tables to MANIFEST to CURRENT | Replay policy, authoritative metadata, file versus namespace durability |
| Flush/compaction to installation to cleanup | Safe entry drops, output protection, retained versions, obsolete-file eligibility |

Every P1 finding must have corrected or added source-local contracts.
Every P2 finding must have either a bounded teaching improvement or a
documented retain/no-change rationale. The review should report no
unexplained new contract, no altered runtime behavior, no stale navigation,
and no lost machine-readable annotation.

Success is a reader being able to follow and explain these routes, not
a target count of comment lines.

## Independent delivery review

On 2026-10-10 an independent static review covered the full source/documentation
diff. Two demonstrated comment inaccuracies were accepted and corrected:

1. `Table::Open` always checks footer/index topology, but metaindex/filter
   validation requires a configured filter policy. The declaration now states
   this condition rather than implying unconditional validation.
2. Shadowed entries of either kind can be dropped without a deeper-level check.
   An otherwise unshadowed tombstone requires visibility to the oldest snapshot
   and no deeper value to hide. Implementation and declaration wording now
   distinguish those independent rules.

No findings were declined. A focused independent follow-up confirmed both
corrections with no residual inaccuracies identified. After the fixes, the
core target was rebuilt, all 46 selected `CompactionTest`/`TableTest` cases
passed, and token/directive, formatting, and documentation checks were repeated.
This review is bounded to the diff and these corrections, not a claim of
exhaustive behavioral verification. The PR records remote CI and merge evidence.

## Local validation record

Validated on 2026-10-10 in the local working tree before creating the delivery
commit or pull request.

| Check | Scope | Result |
|---|---|---|
| Source-only change | All 252 baseline files; 82 changed sources | C++ non-comment token streams preserved; Python syntax trees and non-comment tokens preserved; no new docstrings. |
| Machine annotations | Coverage, linter, and formatter controls | Preserved exactly. Added lines can still shift source-location diagnostics. |
| Formatting | 79 changed C++ files and final diff | Existing formatter's dry-run and `git diff --check` passed. |
| Scope and navigation | All dispositions and new source lesson/reference paths | Every baseline file appears exactly once with the matching disposition; paths resolve and source-tour entry symbols remain present. |
| Documentation | Audit, plan, style, and development index | Documentation checker passed; all 13 existing checker tests passed. |
| Core/native tool consumers | Existing Windows Debug unit, public API, allocation, compression-failure, and tool targets | Built successfully; all 708 unit-tier CTest entries passed. |
| Crash-model evidence | Existing Windows compatibility crash target | Built successfully; five selected `CrashFileSystemTest`/`PowerLossTest` cases passed. |
| Profiling/diagnostic consumers | Existing selected-workload, read-diagnostic, native collector, and calibration targets | Built successfully; six selected contract/parser/smoke/protocol CTest entries passed. |
| Ordinary benchmark consumer | Existing Windows benchmark target | Built successfully; no throughput result or speedup claim is made. |

The Windows checks do not constitute a POSIX, sanitizer, full fuzz, or native
power-loss campaign. Token preservation and executable evidence guard against
accidental code changes; they do not prove comment accuracy or teaching
clarity. Those were checked locally against implementations/callers and in
the independent review above; remote checks and merge remain PR delivery gates.

## Final file dispositions

This appendix accounts for all 252 files in the fixed audit scope. **C**
means source comments changed (82 files); **R** means existing explanatory
comments retained with the stated rationale (69); **N** means no additional
explanation is needed at that location (101). N does not mean the underlying
algorithm is trivial: its contract can belong in a shared header, lesson,
validator, or named test instead.

Each filename is relative to its directory column. Machine-readable
annotations remain intact in every category. These are comment-review
dispositions, not claims of exhaustive behavioral verification.

| Directory | Disposition | Files | Change or retain/no-addition rationale |
|---|---|---|---|
| `.github/workflows` | N | `ci.yml`, `hardening.yml` | Named jobs, dependency edges, and executable checks already express the gates; contributor procedures belong in the development guide. |
| `(root)` | R | `CMakeLists.txt` | Keep the explicit separation of ordinary throughput and read instrumentation. |
| `benchmarks` | C | `db_bench.cc`, `profiling_bench.cc`, `windows_cpu_profile.cc`, `windows_profile_calibration.cc`, `windows_profile_protocol.h`, `windows_profile_workload.h` | Explain preparation, warmup, fixed work, measured epochs, CPU attribution, process ownership, and verification boundaries. |
| `benchmarks` | N | `CMakeLists.txt` | Target/policy/test names expose platform selection; the guide owns measurement procedures and validators own report schemas. |
| `cmake` | R | `Crc32cDependency.cmake`, `GoogleBenchmark.cmake`, `LevelDbReference.cmake` | Retain useful dependency, version-normalization, and reference-provenance explanations. |
| `cmake` | N | `CompilerWarnings.cmake`, `CompressionDependencies.cmake`, `ExposeLevelDbMmapLimit.cmake`, `ReferenceIdentity.cmake`, `RefreshBenchmarkPolicy.cmake`, `WindowsBackend.cmake` | Explicit configuration checks and target names expose the controls; avoid a duplicate procedural narrative. |
| `fuzz` | C | `database_fuzzer.cc`, `format_fuzzer.cc` | State command/model/decode-budget limits and distinguish self-consistency from independent format or durability evidence. |
| `fuzz` | N | `CMakeLists.txt`, `fuzz_support.h` | Build gates and the small assertion helper are self-descriptive; target scope is documented at the fuzzer entry points. |
| `include/modern_leveldb/base` | C | `bytes.h`, `coding.h`, `crc32c.h`, `hash.h`, `result.h` | Connect borrowed bytes, persistent coding, non-cryptographic checks, errors, and absence to their concepts. |
| `include/modern_leveldb/base` | R | `comparator.h` | Keep the existing ordering, identity, callback, and lifetime obligations. |
| `include/modern_leveldb` | C | `db.h`, `iterator.h`, `options.h`, `snapshot.h`, `write_batch.h` | Make ownership, atomic visibility, absence, concurrency, moved-from state, and durability controls usable from public declarations. |
| `include/modern_leveldb` | R | `database_state.h` | Preserve the distinction between an owning observation and live database or snapshot state. |
| `src/api` | R | `api_internal.h` | Keep child retention and reverse-destruction-order reasoning. |
| `src/api` | N | `database.cc`, `iterator.cc`, `snapshot.cc`, `write_batch.cc` | Facade forwarding and handle checks implement the public contracts; do not repeat those declarations in each wrapper. |
| `src/base` | C | `coding.cc` | Explain terminal varint-bit truncation as the local decoder rule. |
| `src/base` | N | `comparator.cc`, `crc32c.cc`, `hash.cc` | Header contracts and focused codec/hash tests supply the needed meaning; avoid narrating straightforward arithmetic or forwarding. |
| `src/cache` | C | `sharded_lru_cache.h`, `sharded_lru_cache_core.cc`, `sharded_lru_cache_core.h` | Explain ownership transfer, borrowed pins, soft capacity, list membership, and reclamation outside the shard lock. |
| `src/diagnostics` | C | `dump_file.cc`, `dump_file.h` | State offline-input, physical-history, and limited-certification boundaries. |
| `src/diagnostics` | R | `dump_command.h`, `posix_output.h`, `windows_output.h` | Keep command/output ownership and safety contracts. |
| `src/diagnostics` | N | `dump_command.cc`, `posix_output.cc`, `windows_output.cc` | Argument handling and output wrappers do not need another copy of the command or filesystem contract. |
| `src/engine` | C | `compaction.cc`, `compaction.h`, `compaction_picker.cc`, `compaction_picker.h`, `database.cc`, `database.h`, `db_iterator.h`, `flush.cc`, `flush.h`, `iterators.cc`, `lookup.cc`, `lookup.h`, `recovery.cc`, `seek_statistics.cc`, `seek_statistics.h`, `write_path.cc`, `write_path.h` | Explain first-source visibility, retained read sources, grouping/publication, safe maintenance, replay policy, and cleanup without upstream-name shortcuts. |
| `src/engine` | R | `build_table.cc`, `build_table.h`, `db_iterator.cc`, `internal_iterator.h`, `iterators.h`, `recovery.h`, `table_cache.cc`, `table_cache.h` | Preserve existing build/recovery/lifetime contracts and iterator invariants; companion declarations now provide the shared concept map. |
| `src/format` | C | `internal_key.h`, `wal_format.cc`, `wal_format.h`, `write_batch.cc`, `write_batch.h` | Explain packed order, checked/trusted domains, borrowing, framing, and the exact-header boundary. |
| `src/format` | N | `internal_key.cc` | Representation and move/view rules belong in the header; named codecs and focused tests expose their implementation details. |
| `src/instrumentation` | C | `read_diagnostics.h` | Distinguish nested inclusive durations from additive exclusive costs and throughput. |
| `src/instrumentation` | N | `read_diagnostics.cc` | Session/stage ownership is documented in the header; counters and scoped timing implement it directly. |
| `src/memory` | C | `arena.cc`, `arena.h`, `memtable.cc`, `memtable.h`, `skiplist.h` | Explain monotonic storage, reserved-byte accounting, one-writer publication, trusted entries, visibility, and borrowing pins. |
| `src/metadata` | C | `version_edit.h`, `version_set.cc` | Explain canonical edits, allocation-before-I/O, persistence-before-publication, and separate name/data barriers. |
| `src/metadata` | R | `filenames.cc`, `filenames.h`, `version.h`, `version_edit.cc`, `version_set.h` | Retain parsing, version/topology, edit, and installation contracts that already express the local rules. |
| `src/metadata` | N | `version.cc` | Version ownership/order is explained in the header; this implementation needs no duplicate narrative. |
| `src/platform` | C | `background_executor.h`, `file_system.h`, `posix_file_system.h`, `serial_executor.cc`, `windows_file_system.h` | State scheduling, teardown, short-read, append, mapping, synchronization, and admitted durability contracts. |
| `src/platform` | R | `clock.cc`, `windows_file_system.cc`, `windows_file_system_internal.h` | Retain local native timing, I/O, mapping-fault, and resource-lifetime explanations. |
| `src/platform` | N | `clock.h`, `file_system.cc`, `mapped_read_limiter.h`, `path.cc`, `path.h`, `posix_file_system.cc` | Shared backend contracts and named primitives/tests explain these helpers; do not add commentary to every system call or accessor. |
| `src/table` | C | `block.cc`, `block.h`, `block_builder.h`, `bloom_filter.cc`, `bloom_filter.h`, `filter_block.h`, `table.h`, `table_builder.h` | Explain restart reconstruction, lazy validation, owned/borrowed backing, probabilistic exclusion, index separators, and exception recovery limits. |
| `src/table` | R | `block_builder.cc`, `block_format.h`, `compression.cc`, `compression.h`, `filter_block.cc`, `table.cc`, `table_builder.cc` | Keep existing format, compression, filter, and traversal reasoning; header maps cover the shared learning entry. |
| `src/table` | N | `block_format.cc` | Trailer/checksum contracts and representation tests already explain the small codec implementation. |
| `src/wal` | C | `wal_io.h` | Separate logical events, benign crash tails, terminal errors, poisoned writes, borrowed records, and reopen padding. |
| `src/wal` | R | `wal_io.cc` | Preserve existing framing/reassembly and first-error implementation explanations. |
| `tests` | R | `CMakeLists.txt` | Keep existing target/coverage and test-gate rationale. |
| `tests/cmake/benchmark_declaration` | N | `CMakeLists.txt` | Fixture target and assertion names express the declaration contract. |
| `tests/cmake/benchmark_declaration/source` | N | `CMakeLists.txt` | Minimal fixture setup is clearer without a repeated explanation. |
| `tests/cmake/consumer` | R | `CMakeLists.txt` | Preserve the consumer-contract note alongside its build assertions. |
| `tests/cmake/consumer` | N | `main.cc` | A small compilation consumer needs no commentary beyond its tested API use. |
| `tests/cmake/consumer/modules` | N | `CompilerWarnings.cmake` | The fixture helper is self-descriptive. |
| `tests/cmake` | N | `profile_capabilities.cmake`, `reference_control.cmake`, `reject_reference.cmake`, `required_tests.cmake` | Named scenarios, explicit checks, and failure messages carry the expected build/control behavior. |
| `tests/cmake/reference_declaration` | N | `CMakeLists.txt` | The dependency-declaration fixture states its assertions directly. |
| `tests/cmake/reference_provider` | N | `CMakeLists.txt` | The provider fixture states its assertions directly. |
| `tests/compatibility` | N | `database_differential_test.cc`, `golden_database_test.cc` | Named cross-engine/cross-open checks and retained external fixtures provide the oracle; do not annotate every assertion. |
| `tests/crash` | C | `power_loss_test.cc` | Explain mandatory acknowledged durable prefixes and allowed whole-batch recovery states. |
| `tests/crash` | N | `process_crash_test.cc`, `process_crash_windows_test.cc` | Named native crash checks and shared process helpers expose the scenario; simulation interpretation belongs at the model boundary. |
| `tests/extended` | N | `CMakeLists.txt` | Tier/target names and platform selections expose the test composition. |
| `tests/model` | N | `database_model_test.cc` | Independent map-oracle operations and scenario names are readable without line-by-line narration. |
| `tests/support` | R | `crash_file_system.h`, `manual_clock.h`, `manual_executor.h`, `memory_file_system.h`, `native_file_system.h`, `scripted_iterator.h`, `windows_crash_child.cc`, `windows_crash_process.cc` | Preserve fault-model, scheduling, backing-resource, scripted-error, and owned-process caveats. |
| `tests/support` | N | `crash_file_system.cc`, `crash_observation.h`, `database_model.h`, `diagnostic_fixture.cc`, `memory_file_system.cc`, `reference_directory.h`, `table_file.h`, `temporary_directory.h`, `windows_crash_process.h`, `windows_lock_child.cc` | Shared helper contracts and descriptive operations cover these fixtures; avoid duplicating scenarios or API contracts. |
| `tests/tools` | R | `benchmark_policy_test.py`, `diagnostic_tool_test.py`, `performance_smoke.py`, `read_diagnostics_smoke.py`, `windows_profile_test.py` | Retain focused policy, process, schema, and platform explanations. |
| `tests/tools` | N | `benchmark_gate_test.py`, `check_documentation_test.py`, `ci_changes_test.py`, `performance_test.py`, `profile_process_test.py`, `profile_report_test.py`, `write_parity_test.py` | Scenario names and explicit report/control assertions express their expected behavior; execution maps belong in the production tools. |
| `tests/unit/api` | N | `public_api_native_test.cc`, `public_api_test.cc`, `public_api_windows_test.cc`, `public_api_without_posix_test.cc` | Public declarations now give the caller rules; descriptive behavior tests remain the evidence. |
| `tests/unit/base` | N | `bytes_test.cc`, `coding_test.cc`, `comparator_test.cc`, `crc32c_test.cc`, `hash_test.cc`, `result_test.cc` | Named representation/error/ordering cases and literal expectations need no redundant annotations. |
| `tests/unit/cache` | N | `cache_allocation_test.cc`, `sharded_lru_cache_test.cc` | Named transfer, pin, capacity, replacement, and concurrency cases already expose the behavior. |
| `tests/unit/diagnostics` | R | `dump_file_test.cc` | Preserve the targeted malformed/physical-file explanation. |
| `tests/unit/diagnostics` | N | `dump_command_test.cc`, `posix_output_test.cc`, `windows_output_test.cc` | Named command/output cases and assertions carry the contract. |
| `tests/unit/engine` | C | `compaction_picker_test.cc`, `iterators_test.cc`, `recovery_test.cc`, `seek_statistics_test.cc` | Explain boundary-history failure, equal-entry direction changes, empty-batch sequences, and deterministic sampling locally. |
| `tests/unit/engine` | R | `build_table_test.cc`, `compaction_test.cc`, `database_test.cc`, `db_iterator_test.cc`, `flush_test.cc`, `lookup_test.cc`, `table_cache_test.cc`, `write_path_test.cc` | Keep useful targeted schedule, ordering, overlap, and failure explanations instead of annotating every test. |
| `tests/unit/engine` | N | `database_native_test.cc`, `database_windows_test.cc`, `database_without_posix_test.cc` | Named platform/engine cases already identify the tested boundary. |
| `tests/unit/format` | C | `internal_key_test.cc` | Decompose the independent trailer bytes instead of relying only on encoder/decoder agreement. |
| `tests/unit/format` | N | `wal_format_test.cc`, `write_batch_test.cc` | Named fragmentation, malformed-batch, and trusted-equivalence cases expose their expected outcomes. |
| `tests/unit/memory` | N | `arena_test.cc`, `memtable_test.cc`, `skiplist_test.cc` | Named accounting, visibility, and publication cases are executable evidence for the new header contracts. |
| `tests/unit/metadata` | R | `version_edit_test.cc`, `version_set_test.cc`, `version_test.cc` | Keep targeted wire-format and topology/install-order explanations, fixture provenance, and machine annotations. |
| `tests/unit/metadata` | N | `filenames_test.cc` | Literal name/parse cases are already readable. |
| `tests/unit/platform` | R | `serial_executor_test.cc` | Retain the deliberate scheduling/teardown explanation. |
| `tests/unit/platform` | N | `clock_test.cc`, `crash_observation_test.cc`, `file_system_test.cc`, `posix_file_system_test.cc`, `reference_directory_test.cc`, `windows_file_system_test.cc` | Named time, ownership, I/O, and platform cases provide evidence without duplicating the shared backend contract. |
| `tests/unit/table` | C | `block_format_test.cc`, `block_test.cc`, `bloom_filter_test.cc`, `table_builder_test.cc`, `table_test.cc` | Explain trailer/restart arithmetic and independent fixture origins; describe binary probes without upstream-comparison prose. |
| `tests/unit/table` | R | `compression_failure_test.cc`, `filter_block_test.cc` | Preserve focused failure/filter-region explanations and formatter controls. |
| `tests/unit/table` | N | `compression_test.cc`, `table_native_test.cc`, `table_posix_test.cc` | Named round-trip, mapping, and native lifetime cases are already readable evidence. |
| `tests/unit` | N | `toolchain_test.cc` | A minimal toolchain contract test needs no additional prose. |
| `tests/unit/wal` | R | `wal_io_test.cc` | Keep targeted fragment/tail/error scenarios. |
| `tests/unit/wal` | N | `wal_io_native_test.cc` | Native reopen cases directly exercise the shared stream contract. |
| `tools` | C | `profile_report.py`, `run_performance.py`, `run_write_parity.py` | Explain owned-process cleanup, fixed-work controls, frozen experiment identity, and binding validated artifact bytes. |
| `tools` | R | `check_benchmark.py`, `check_documentation.py`, `ci_changes.py`, `run_fuzz.py` | Existing checker/router/runner notes remain useful; executable validators remain authoritative for exact policies and schemas. |
| `tools` | N | `CMakeLists.txt`, `modern_leveldb_tool.cc` | Target setup and the small command facade need no duplicate execution map. |
