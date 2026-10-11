# Textbook Improvement Plan

## Completion goal

Complete the agreed code-improvement work as a sequence of small, independently
reviewable changes. The result should expose the engine's responsibilities as
textbook-style function compositions, without production runtime assertions or
fine-grained read-diagnostic branches.

Completion means all tasks below, including accepted child refactors from the
composition inventory, have evidence and persistent commits. No known actionable
review finding or failed required check may be hidden behind a completed status.
This is a refactoring goal, not a promise to implement every future database
feature or to split every long function.

The scope is repository-owned production code, public headers, tests, benchmarks,
tools, build integration, and directly related documentation. External
dependencies, generated files, and unrelated features are excluded.
[ADR-0076](../adr/0076-textbook-source-composition.md) defines the preserved
behavior and source-style policy.

## Task execution contract

Only one task is actively being implemented at a time. A task changes one named
responsibility and its directly dependent declarations, tests, and documentation.
If its explanation needs several independent responsibilities, split it before
implementation. Do not use a mechanical function-line limit.

Each task follows this sequence:

1. Read the actual callers and record the local design, ownership, errors,
   invariants, exclusions, and acceptance checks.
2. Add or retain executable characterization. For a new contract, observe the
   focused test fail before implementing it; do not manufacture failures for
   an unchanged behavior.
3. Make the smallest complete change and run the focused checks.
4. Review the diff, resolve demonstrated defects, and record the outcome.
5. Persist the slice in a small commit before starting the next implementation.

The owner requires separate independent persona reviews for design documents
and implementation changes, including low-risk tasks in this backlog. Publish
and merge the reviewed design-only PR before implementing its new contract.
Then review the code with a separate implementation persona and publish a narrow
implementation PR. A shared accepted design may cover multiple named slices;
each implementation still needs its own code review.

Local commits do not imply a published, reviewed, or merged pull request.
The owner has authorized sequential PR publication and merging when there are
no unresolved actionable review findings and all applicable checks pass on the
exact published head. Use the `algorithm-apprentice` GitHub account and commit
identity. Never bypass a failing check or use administrative merge overrides.
Delete merged local and remote topic branches; retain branches or saved work
that still contain undelivered changes.

Follow [ADR-0006](../adr/0006-sequential-pull-request-workflow.md). The owner
explicitly permits dependent local preparation while the preceding PR's CI
runs. Keep it on a separate branch, do not change the published head, and do not
publish the dependent PR before the predecessor merges. Reapply the local slice
to the merged base and validate that revision before publication. This limited
overlap does not authorize parallel unrelated implementation branches.

Existing locally prepared work predates this delivery rule. Review its design
separately, merge that design before publishing implementation, and disclose
that the design review is retrospective rather than claiming a pre-implementation
gate occurred.

Do not run a large orchestration workflow or make parallel implementation
branches merely because the backlog has many items. A blocked task stays blocked;
its failure is not a reason to broaden the refactor. There is no speculative
calendar deadline: progress is measured by verified task outcomes.

## Ordered backlog

The rows are delivery order, not a claim that every adjacent pair has a technical
dependency. Dependent source and test changes remain in the same small task.

| ID | Small task | Acceptance boundary | Initial state |
|---|---|---|---|
| P01 | Persist policy and the bounded backlog | ADR and style guide agree with this plan | Done |
| A01 | Remove foundation assertions | Foundation, cache, memory, format, and WAL operations retain their side effects | Delivered in #165 |
| A02 | Finish table assertion cleanup | Remove assertion-only builder state; retain corruption and terminal-state checks | Delivered in #166 |
| A03 | Finish metadata assertion cleanup | Remove debug verification only; retain ordered merge and MANIFEST installation | Prepared, not complete |
| A04 | Finish platform and API assertion cleanup | Retain mapping-slot restoration, explicit validation, and independent termination contracts | Prepared, not complete |
| A05 | Finish engine assertion cleanup | Retain locks, publication, snapshot ownership, queue completion, and sequence restoration | Prepared, not complete |
| A06 | Remove the trusted-status assertion helper | Replace all callers with explicit discard without skipping operations; scan non-instrumentation production code | Prepared, not complete |
| D01 | Retire diagnostic entry points | Remove dedicated targets, CLI/report/runner paths, and their tests coherently; ordinary performance remains usable | Pending |
| D02 | Remove API, key-format, and lookup hooks | Delete diagnostic-only selection timers and parameters, not read-sampling behavior | Pending |
| D03 | Remove table-cache and SSTable hooks | Preserve cache, filter, decoding, checksum, and copied/mapped behavior | Pending |
| D04 | Remove native filesystem hooks | Preserve POSIX and Windows acquisition, cleanup, and error contracts | Pending |
| D05 | Delete the unused instrumentation leaf | Delete its two source files and remaining build macros; no live target or include refers to them | Pending |
| D06 | Reconcile diagnostic documentation | Current docs advertise supported paths only; historical rationale links to retirement | Pending |
| F01 | Inventory API and engine orchestration | Name concrete mixed-responsibility functions, or record why no change is needed | Pending |
| F02 | Inventory storage and recovery orchestration | Review table, WAL, metadata, and recovery; identify complex algorithms to keep intact | Pending |
| F03 | Inventory foundation and platform composition | Review base, cache, memory, native I/O, and offline diagnostics without inventing wrappers | Pending |
| F04 | Inventory benchmark and tool composition | Review repository-owned harnesses and scripts separately from engine algorithms | Pending |
| F05 | Freeze and execute accepted child refactors | Each accepted candidate gets a named task and separate design, checks, and commit | Pending |
| L01 | Reconcile learning and architecture guidance | Source tours, contracts, and examples match the implemented function boundaries | Pending |
| V01 | Verify the ordinary product | Required build, unit, API, and build-integration checks pass in the configured toolchain | Pending |
| V02 | Verify storage behavior | Model, compatibility, and crash checks pass in their supported environment | Pending |
| V03 | Verify retained performance tooling | Ordinary performance contracts/smoke and native CPU capture/calibration remain supported | Pending |
| V04 | Verify docs and removal boundaries | Tool/doc checks pass; no production runtime assertion, retired macro, or diagnostic target remains | Pending |
| V05 | Complete independent review and handoff | Resolve actionable findings, record environment gaps and evidence, persist the final outcome | Pending |

Assertion preparation has already touched several source areas in one working
tree. Those edits are not completed tasks: separate them into the boundaries
above, audit assertion-only members, and validate each resulting commit. Do not
commit the entire prepared diff as one large change.

For D01, retire the executable, build/test registrations, benchmark diagnostic
mode, Python runner/validator, and directly dependent tests together. Keep the
ordinary macro-zero build temporarily while D02-D04 remove source hooks.
Delete the orphaned instrumentation leaf and macro definitions only in D05.
Ordinary report provenance may retain the constant
`read_diagnostics_compiled=false`; it is not a remaining instrumentation branch.

## Function-composition inventory

F01-F04 are review tasks, not authorization for four broad rewrites. Record each
source area's disposition as either a concrete candidate or "keep as is" with
its reason. Include the function, mixed responsibilities, proposed names,
retaining owners, synchronization/error boundaries, affected tests, and estimated
change surface. Review the related tests and examples for readability too.

F05 freezes the accepted candidate list before those implementations begin.
Assign each candidate a child ID such as F05-01. If a candidate still contains
independent responsibilities, split it into smaller child tasks first. Append
the exact child tasks and outcomes here; do not mark F05 complete just because
the inventory exists.

Accept a refactor only when the named stages make the existing behavior easier
to understand. Keep tightly coupled complex algorithms intact and document that
decision. Do not add generic callback frameworks, duplicate validation, or
one-line forwarding helpers solely to make a function shorter.

After the frozen list is delivered, unrelated newly discovered improvements
belong to a later backlog. Required fixes for defects introduced by this work
remain part of the current completion goal.

## Evidence and status

The shared-rule changes in ADR-0075 and the source-style policy were committed
locally before sequential PR delivery began. The shared-rule design merged in
#160; read-option preparation merged in #161, level-iterator assembly in #162,
and background-installation decisions in #163. Each implementation received
a separate code-persona review and passed its remote CI checks before merging.
Remaining local commits are not evidence of remote delivery.

A01 was originally committed locally as `c09c7e6`; its focused foundation run
passed 187 tests. After design #164 merged, the isolated implementation was
reapplied to that base. A separate C++ invariant/resource-lifecycle persona
identified no actionable code findings. The rebased warnings-as-errors MSVC
Debug build and 711 unit-labeled checks passed. Seven assertion-only death tests
were removed; supported behavior tests and static assertions remain. All 20 remote CI checks passed and the implementation merged in #165.
Its local and remote topic branches were deleted.

A02 merged in #166 after an independent SSTable format/iterator/state review,
711 passing local unit checks, and all 20 remote CI checks. Its topic branches
were deleted.

A03 has an independent topology/MANIFEST code review with no actionable
findings. Reapplying only the three metadata files to merged #166 preserved the
reviewed source/test tree. The warnings-as-errors MSVC Debug build and 711
unit checks passed on that revision; remote CI and merge remain required.

A04-A06 are separate locally committed preparation, not delivered changes.
An earlier combined CTest run reported 12 build-integration failures or not-run
cases outside the MSVC environment. A later isolated run under `vcvars64`
passed all 13 integration checks. This does not replace exact-head remote
checks for subsequent PRs.

Use task status and this document's outcome entries as the handoff, not memory
of previous conversation turns. Record actual checks after execution; do not
reuse the old baseline's test counts as evidence for the new revision. Removed
assertion-only death tests legitimately change the count.

Windows checks do not establish POSIX correctness or sanitizer coverage.
Environment-specific checks must run in their admitted environment; an
unavailable environment remains an explicit gap, not a passing result.
