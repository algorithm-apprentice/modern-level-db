# ADR-0076: Textbook Source Composition

- Status: Implemented
- Date: 2026-10-11

## Concept and contract

```text
Concept:
    Present the learning-oriented engine as compositions of readable algorithms.
Observed mismatch:
    Runtime assertions and optional read-diagnostic preprocessor branches
    interrupt the core algorithm, while some orchestration hides its stages.
Current authority:
    The code-style guide, ADR-0004, ADR-0057, and ADR-0074.
Invariant that must remain:
    Preserve supported API behavior, external validation, ownership,
    publication, durability, ordinary benchmarks, and CPU profiling.
```

The project owner confirmed that fine-grained read diagnostics should be
removed completely, including its dedicated build and scripts. Ordinary
benchmarks and CPU profiling remain supported. This decision does not remove
offline storage-file diagnostics.

## Function composition

Prefer small functions with one named responsibility. An orchestration function
should present a few meaningful stages, each implemented by a separate function.
Decompose a stage again when it still mixes independent responsibilities.

Keep a complex algorithm together when splitting its tightly coupled loop state
would make it harder to understand. Function length is evidence of a possible
problem, not a numeric limit. Do not introduce a generic callback framework or
one-line wrappers merely to reduce line counts.

This is the current source-style authority. It does not require a repository-wide
rewrite in one change; each subsequent task applies it to its bounded source
area. The shared-rule slices in ADR-0075 remain valid.

## Runtime assertions

Remove production `assert` statements and their unused `<cassert>` includes.
Preserve `static_assert`, test assertions, explicit public/API argument validation,
file-format corruption checks, I/O errors, and supported-domain exceptions.
Do not replace an assertion with a repeated recoverable check on a trusted path.

Headers continue stating caller preconditions. Invalid calls to trusted internal
helpers remain outside the supported domain; Debug no longer promises an
assertion-driven abort. Remove death tests whose only purpose was that promise.
Retain death tests for independently supported termination contracts.

Audit assertion-only work before deletion:

- Drop debug-only VersionBuilder live-file verification, not its real edit
  application or ordered merge.
- Drop BlockBuilder debug size calculations, not its encoded bytes.
- Drop SkipList duplicate verification, not predecessor search.
- Preserve atomic counter restoration, cache-index removal, and other state
  transitions whose return value was asserted.
- Remove unused verification counters and temporary values, rather than hiding
  them with `maybe_unused`.
- Replace the internal `AssertSuccess` wrapper with explicit result discard at
  callers whose trusted success precondition was already established. The
  called operation still executes.

Remove assertion-only private state when it no longer affects supported
behavior. Do not remove state used for a documented terminal or cleanup policy.

## Read-diagnostic removal

Choose the existing ordinary (`MODERN_LEVELDB_READ_DIAGNOSTICS=0`) branch at each
conditional and remove the conditional itself. Audit remaining diagnostic-only
wrappers and parameters; do not leave no-op timing abstractions in algorithms.

Remove:

- `src/instrumentation/read_diagnostics.h` and `.cc`;
- the separately compiled `modern_leveldb_read_diagnostic` library;
- the `modern_leveldb_read_diagnostics` executable and its smoke test;
- fixed-count diagnostic-report generation, validation, and runner mode.

Preserve:

- ordinary throughput, selected read and mutation workloads;
- copied/mapped read controls and pinned-reference controls;
- build provenance, workload completion reports, and owned process cleanup;
- CPU capture markers, native Windows collector/calibration, and symbol metadata;
- offline `modern_leveldb_tool dump` storage-file diagnostics.

Retain the ordinary provenance field `read_diagnostics_compiled=false` as a
constant where existing throughput/write-parity report schemas require it.
It no longer selects a code branch or build target. Historical ADRs retain their
measurement rationale, but links to removed sources must point to this retirement
decision. Current operational docs must not advertise the removed executable.

## Small execution slices

Follow the bounded backlog in the
[textbook improvement plan](../development/textbook-improvement-plan.md).
The project owner requested smaller tasks than the original component-wide
slices. Complete one named responsibility at a time, with its evidence and
commit boundary, before starting the next implementation task.

The owner subsequently authorized combining the remaining mechanical assertion
and diagnostic deletions into one cleanup PR, rather than a PR per component.
The task IDs remain audit and verification boundaries, not mandatory PR counts.
This delivery adjustment does not change the preserved contracts or authorize
unrelated function-composition rewrites in the cleanup.

First finish and separate the existing uncommitted assertion cleanup. Retire
the diagnostic executable and runner coherently before deleting its unreachable
hooks in smaller source-area tasks. Do not leave a runnable diagnostic program
that silently reports partial measurements.

Review function composition separately from instrumentation removal. Inventory
concrete candidates and preserved complex algorithms, then give each accepted
candidate its own bounded design, implementation, and verification task. Do not
combine an unrelated large-function rewrite with these removals.

## Evidence

The previous shared-rule revision provides a clean baseline: 718 Windows Debug
unit tests and 18 model/compatibility/crash tests passed before this cleanup.

For assertion removal, review every expression and its surrounding statements,
then run the affected unit suites with warnings as errors. Source scans must show
no production runtime assertions; static assertions and boundary checks remain.

For diagnostic removal, first characterize the removed CLI modes as unsupported
while keeping ordinary workload/report contracts. Build and run the ordinary
product, performance contracts/smoke, and native CPU collector/calibration tests.
Run Python tool tests and the documentation checker. Source/build scans must show
no diagnostic macro or dedicated target remains.

Record final counts and actual commands only after validation. Non-Windows
sanitizers and GCC coverage still require their normal delivery environments.
