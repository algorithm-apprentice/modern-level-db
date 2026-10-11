# ADR-0077: Bounded Orchestration Composition

- Status: Implemented
- Date: 2026-10-12

## Concept and contract

```text
Concept:
    Teach coordination as named stages without hiding ownership or algorithms.
Observed mismatch:
    Obsolete-file cleanup mixes retention policy with collection and deletion;
    recovery embeds fresh-WAL durability inside its orchestration;
    the performance runner embeds workload command construction in its lifecycle.
Invariant that must remain:
    Preserve lock boundaries, operation ordering, errors, resource ownership,
    fixed-work budgets, schemas, and the supported measurement controls.
```

This is the frozen F05 child list required by
[ADR-0076](0076-textbook-source-composition.md) and the
[improvement plan](../development/textbook-improvement-plan.md).
The mechanical cleanup is already delivered in #167. None of these children
changes behavior, adds runtime assertions, or restores read instrumentation.
The design is reviewed and delivered before implementing these children.

## F05-01: Obsolete-file retention and collection

`DatabaseEngine::RemoveObsoleteFiles` currently snapshots live file numbers,
lists children, classifies file retention, evicts cached tables, and deletes
paths with the mutex released.

Extract a file-local `KeepFile` predicate taking a parsed filename, the live
number set, and current log/MANIFEST thresholds. Its decision is unchanged:
retain logs/descriptors at or above their thresholds, tables/temporary files
in the live set, and LOCK/CURRENT unconditionally. Unknown names are skipped
before calling the predicate.

Extract private `CollectObsoleteFiles`, called with the mutex held. It snapshots
all pinned Versions and pending outputs, lists the directory, applies the
predicate, evicts obsolete tables before recording their paths, and returns
`Result<std::vector<std::filesystem::path>>`. Preserve directory iteration order.

The caller retains the sticky-error early return and the explicit
unlock/delete/relock sequence. Directory-list failure still means no cleanup;
deletion is still best effort. Collection forwards a directory-list error;
the caller returns with the mutex held on failure, matching current behavior.
A successful empty vector remains distinct and still reaches unlock/relock;
changing scheduling is not this task. Review both lock paths explicitly and
retain listing-failure and writes-during-deletion characterization.
Do not add an abstract cleanup framework or change error notification policy.

Change surface: `database.h`, `database_background.cc`, and directly related
learning guidance. Retain characterization in lifecycle/background/iterator
tests: obsolete and unknown files, listing failure, sticky error, pinned
Versions, pending outputs, and writes during unlocked deletion.

## F05-02: Recovery WAL acquisition and durability

Extract file-local `CreateRecoveryLog` in `recovery.cc`. It receives filesystem,
directory, already-reserved log number, and the existing durability option.
It opens the writable file, constructs the owning `WalWriter`, and, when enabled,
syncs the file before syncing the directory. Return
`Result<std::unique_ptr<WalWriter>>`, forwarding the original error unchanged.
Local RAII owners remain alive through both barriers and release on failure.

`RecoverDatabase` still reserves the number after replay, calls the helper,
sets both log fields in the edit, applies the MANIFEST edit, and only then
calls `BuiltTables::Keep`. Move the writer into the returned database only
after publication. Do not share this helper with runtime WAL rotation:
rotation intentionally has different barriers, sticky-error behavior, and
ownership transfer.

Change surface: `recovery.cc` and the recovery learning tour. Existing tests
already characterize the exact ordinary and weak creation operation sequences.
Run all `RecoveryTest` cases, including fault-injection retry/replay, and
the supported storage/crash tier. Do not invent a failing test for an unchanged
contract.

## F05-03: Performance workload command construction

Extract pure `benchmark_arguments` in `tools/run_performance.py`. It receives
the selected case, output paths, already-validated effective repetition/time
values, smoke flag, and existing Modern/reference controls. It returns only
the benchmark argument list, not an executable or process.

Move the current fixed-work/adaptive time selection, report paths, mandatory
Google Benchmark flags, engine-specific options, and mutation-only smoke flag
into the helper in their existing order. Reuse `mutation_specification` and
`case_parts`; do not duplicate control validation or add a configuration class.
The coordinator keeps validation/defaulting before output creation, manifest
creation, executable copying, process ownership, capture-only profile markers,
report validation, provenance binding, and `try/finally` cleanup.

Add focused pure-helper tests before implementation: exact read arguments,
normal versus smoke mutation budgets, applicability of each engine control,
and fresh returned lists. Observe the missing-helper test failure, then run
ordinary contracts/smoke/mutations and native CPU capture. The generated
commands and completion/report schemas must not change.

Change surface: `run_performance.py`, `performance_test.py`, and the profiling
development guide. This is not a redesign of the complete capture lifecycle.
The remaining lifecycle keeps its cohesive artifact/process/error transaction;
future readability work outside this frozen scope belongs to a new backlog.

## Inventory dispositions and exclusions

The plan records F01-F04's inspected flows and keep decisions. In particular,
retain WAL fragment decoding, ordered MANIFEST reconstruction, block seek,
compaction merge, skip-list publication, cache index/LRU mutation, native mapping
acquisition/rollback, and weighted CPU aggregation as their coupled algorithms.
Existing stage helpers already make their ownership and error boundaries clear.
No maximum line count, generic callback layer, or one-line forwarding wrapper
is required.

## Delivery and acceptance

Deliver the three children separately, with separate implementation-persona
reviews and small commits. They share this accepted design but no changed source
files or interfaces; sequential writes in the current enlistment may prepare
the next child while another PR runs CI. Publish independent children from the
merged design base; do not include another child's code in its PR.

For each child, preserve the characterized behavior and resolve all actionable
review findings before pushing. Merge only after all applicable checks succeed
on its exact final head; remove delivered local and remote branches.
Reconcile the plan and learning guidance with actual outcomes after delivery.
Final acceptance includes the integrated ordinary product, storage, performance,
documentation/removal checks, and a separate review of the completion handoff.

## Outcome

The independently reviewed design merged in #168 before implementation.
The three children merged separately in #169, #170 and #171 after separate
implementation-persona reviews and all applicable exact-head checks passed.
The composed tree matches merged main; final integrated validation and the
independently reviewed handoff are recorded in the
[improvement plan](../development/textbook-improvement-plan.md).
