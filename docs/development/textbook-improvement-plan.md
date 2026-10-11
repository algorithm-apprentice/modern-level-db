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
implementation PR. A shared accepted design may cover multiple named slices.
At the owner's subsequent request, the remaining mechanical assertion and
read-diagnostic deletions are consolidated in #167, with an independent review
of the complete combined diff. The internal task IDs are verification boundaries,
not mandatory PR counts. Function-composition changes still use bounded PRs.

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
overlap also permits genuinely independent PRs when their API, ownership, build,
and test dependencies do not overlap. Writes in this enlistment remain sequential.

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

| ID | Small task | Acceptance boundary | Delivery state |
|---|---|---|---|
| P01 | Persist policy and the bounded backlog | ADR and style guide agree with this plan | Done |
| A01 | Remove foundation assertions | Foundation, cache, memory, format, and WAL operations retain their side effects | Delivered in #165 |
| A02 | Finish table assertion cleanup | Remove assertion-only builder state; retain corruption and terminal-state checks | Delivered in #166 |
| A03 | Finish metadata assertion cleanup | Remove debug verification only; retain ordered merge and MANIFEST installation | Delivered in #167 |
| A04 | Finish platform and API assertion cleanup | Retain mapping-slot restoration, explicit validation, and independent termination contracts | Delivered in #167 |
| A05 | Finish engine assertion cleanup | Retain locks, publication, snapshot ownership, queue completion, and sequence restoration | Delivered in #167 |
| A06 | Remove the trusted-status assertion helper | Replace all callers with explicit discard without skipping operations; scan non-instrumentation production code | Delivered in #167 |
| D01 | Retire diagnostic entry points | Remove dedicated targets, CLI/report/runner paths, and their tests coherently; ordinary performance remains usable | Delivered in #167 |
| D02 | Remove API, key-format, and lookup hooks | Delete diagnostic-only selection timers and parameters, not read-sampling behavior | Delivered in #167 |
| D03 | Remove table-cache and SSTable hooks | Preserve cache, filter, decoding, checksum, and copied/mapped behavior | Delivered in #167 |
| D04 | Remove native filesystem hooks | Preserve POSIX and Windows acquisition, cleanup, and error contracts | Delivered in #167 |
| D05 | Delete the unused instrumentation leaf | Delete its two source files and remaining build macros; no live target or include refers to them | Delivered in #167 |
| D06 | Reconcile diagnostic documentation | Current docs advertise supported paths only; historical rationale links to retirement | Delivered in #167 |
| F01 | Inventory API and engine orchestration | Name concrete mixed-responsibility functions, or record why no change is needed | Done; reviewed design #168 |
| F02 | Inventory storage and recovery orchestration | Review table, WAL, metadata, and recovery; identify complex algorithms to keep intact | Done; reviewed design #168 |
| F03 | Inventory foundation and platform composition | Review base, cache, memory, native I/O, and offline diagnostics without inventing wrappers | Done; reviewed design #168 |
| F04 | Inventory benchmark and tool composition | Review repository-owned harnesses and scripts separately from engine algorithms | Done; reviewed design #168 |
| F05 | Freeze and execute accepted child refactors | Each accepted candidate gets a named task and separate design, checks, and commit | Done; #169, #170, #171 |
| L01 | Reconcile learning and architecture guidance | Source tours, contracts, and examples match the implemented function boundaries | Done; child source tours and this handoff |
| V01 | Verify the ordinary product | Required build, unit, API, and build-integration checks pass in the configured toolchain | Done; integrated checks and child CI |
| V02 | Verify storage behavior | Model, compatibility, and crash checks pass in their supported environment | Done; integrated checks and child CI |
| V03 | Verify retained performance tooling | Ordinary performance contracts/smoke and native CPU capture/calibration remain supported | Done; integrated checks and child CI |
| V04 | Verify docs and removal boundaries | Tool/doc checks pass; no production runtime assertion, retired macro, or diagnostic target remains | Done; evidence below |
| V05 | Complete independent review and handoff | Resolve actionable findings, record environment gaps and evidence, persist the final outcome | Done; independent reviews clean and evidence recorded here |

Assertion preparation has already touched several source areas in one working
tree. Audit assertion-only members and retain the small internal commits, but
publish the remaining mechanical deletion as one PR. Do not mix function
composition or behavioral changes into that cleanup.

For D01, retire the executable, build/test registrations, benchmark diagnostic
mode, Python runner/validator, and directly dependent tests together. Keep the
ordinary macro-zero build temporarily while D02-D04 remove source hooks.
Delete the orphaned instrumentation leaf and macro definitions after those hooks
are removed. Preserve frozen write-parity comparisons across historical disabled
definitions and current flagless builds; reject instrumented-only or ambiguous
compile evidence.
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

### Reviewed source inventory

The dispositions below follow actual caller/source/test inspection, not a
function-length ranking. [ADR-0077](../adr/0077-bounded-orchestration-composition.md)
records exact helper names, owners, exclusions, and acceptance for the three
accepted children.

| Area | Inspected flow | Disposition and reason | Characterization |
|---|---|---|---|
| F01 API/read state | `Database::Open`, `Get`, `NewIterator`, `PrepareReadOptions`, `EngineOptions`, `DatabaseEngine::GetState` | Keep: validation, translation, read-option preparation and byte aggregation already have explicit stages; retain borrowed Get versus owning iterator/snapshot lifetimes | Public API, snapshot, iterator, state suites |
| F01 writes/background | `MakeRoomForWrite`, `CommitWrite`, `ApplyBackgroundEdit`, `RemoveObsoleteFiles` | Keep write-pressure/publication state transitions; split obsolete retention/collection as F05-01 | Lifecycle cleanup, sticky error, background scheduling and pinned-Version tests |
| F02 recovery | `RecoverDatabase`, `PrepareLogsForReplay`, `LogReplay` | Keep replay/flush/sequence stages; extract fresh-WAL durability as F05-02 | Recovery exact operation sequences, failure retry and crash cases |
| F02 metadata/table/WAL | `VersionSet::Recover`, `LogAndApply`, `Table::Open`, block seek, `WalReader::ReadNext`/`ReadPhysical`, compaction merge | Keep: ordered reconstruction, validation and state-machine algorithms already compose named decoders/builders; extra wrappers would hide ordering | Version/MANIFEST, table/block, WAL, compaction suites |
| F03 base/format/memory | Fixed/varint coding, write-batch encoding, Arena allocation, SkipList insertion | Keep: shared codecs and allocation/search/publication helpers already separate concerns; tiny forwarding helpers add no concept | Coding, format, arena, skip-list suites |
| F03 cache/native I/O | `ShardedLruCacheCore::Insert`, POSIX/Windows `OpenRandomAccess` | Keep: allocation/index/LRU/retired destruction and pending-map rollback already expose lock/resource stages; platform fallback policies intentionally differ | Cache reentrant destruction, mapping eligibility/rollback/slot tests |
| F03 offline diagnostics | `DumpFile`, `DumpWalFramed`, `RunCommand` | Keep: dispatch, framed reading and record formatting are separate; preserve first-corruption accumulation versus immediate output failure | Diagnostic-tool and dump suites |
| F04 benchmark/CPU | `MutationFixture`, `profile_report.stop_owned`, `summarize_trace` | Keep: fixed-work dispatch, completion and verification are separated; process identity cleanup and weighted aggregation are coupled algorithms | Profile process/parser, mutation stream, CPU calibration/capture suites |
| F04 performance runner | `run_case` | Extract command construction as F05-03; keep the remaining artifact/process/error transaction intact for this bounded goal | Runner invalid-control, fixed-count, artifact ownership and actual workload tests |
| F04 parity/tests/examples | Frozen role validators; related recovery/background fixtures, performance tests and existing public examples | Keep: schemas and named test scenarios express their contracts; no duplicate test framework, example API change or unrelated validation refactor is required | Write-parity CI, existing public consumer and harness checks |

### Frozen F05 child list

| Child | Responsibility | Source change surface | State |
|---|---|---|---|
| F05-01 | Separate obsolete-file retention, locked collection and unlocked deletion | `database.h`, `database_background.cc` | Delivered in #169 |
| F05-02 | Separate fresh recovery WAL acquisition and durability barriers | `recovery.cc` | Delivered in #170 |
| F05-03 | Separate pure workload argument construction from experiment lifecycle | `run_performance.py`, `performance_test.py` | Delivered in #171 |

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
unit checks passed on that revision. Its original #167 head is superseded by the
consolidated cleanup; old-head checks are not evidence for the expanded PR.

A04-A06 and D01-D06 were combined in #167, rather than separate component PRs.
An earlier combined CTest run reported 12 build-integration failures or not-run
cases outside the MSVC environment. A later isolated run under `vcvars64`
passed all 13 integration checks. This does not replace exact-head remote
checks for subsequent PRs.

The consolidated #167 source tree passed the warnings-as-errors MSVC Debug
build and `ctest --test-dir build/windows-debug -L "unit|cmake"`: 710 unit
checks and 13 CMake integration checks. The extended Windows build passed
18 model/compatibility/crash checks. The retained optimized profiling build
passed all 11 performance-labeled checks, including ordinary contracts/smoke,
fixed-work mutations, profile parsing, and native CPU capture/calibration.
All selections used `--no-tests=error` and retained JUnit reports.

The documentation validator and all 13 documentation-tool tests passed.
Three focused frozen-compile tests passed for flagless builds, historical
disabled definitions, and rejection of instrumented-only/ambiguous evidence;
the full write-parity suite remains a non-Windows CI requirement. Generated
Debug/profiling compile commands contain neither the retired collector nor
its macro. Production/include/benchmark/build scans found no runtime assertion,
diagnostic macro, include, or dedicated target.

The delivery amendment passed a separate independent design-persona review.
The integrated implementation persona found a retired runner option that could
bypass rejection when combined with `--help`. Three argument combinations
reproduced that failure. Early rejection fixed it; the full ordinary performance
contract check passed again, and a separate narrow persona confirmed resolution.
No other actionable integrated findings remained. All 20 remote checks passed
on final head `2c339bc02c479a094f0b7790e480583a2c32d7e5`; #167 merged as
`db6ee9d3f6635bea6d51ceb2c4629ceb220a8511`. The merged tree exactly matched
the reviewed head. Stable patch IDs confirmed the seven remaining prepared code
slices were retained. Local/remote delivered branches were deleted and main was
synchronized; the saved pre-delivery stash remains a recovery archive.

### Composition delivery and final acceptance

The frozen inventory and ADR-0077 merged in design-only #168 after independent
design review and all 20 exact-head checks. That review corrected a genuine
planning error before implementation: failed directory listing must return with
the mutex held, whereas a successful empty collection still unlocks/relocks.
A separate narrow design follow-up confirmed the correction.

F05-01 passed its separate implementation-persona review and 122 selected
database/iterator checks before publication. All 20 checks passed on
`ef92addfc5d3067864966f0cdcd2519459b58fea`; #169 merged as
`d538a0a0bdef293a2e3aef05753a745f93ff38e4`.
F05-02 independently passed review and 86 recovery/database checks.
All 20 checks passed on `afe03b4b88e6708078966aed2741fd40885972c7`;
#170 merged as `bd04a481bcf1d9d05788266883c936e126fc9115`.
Both delivered topic branches were removed.

F05-03 added four pure-helper tests first and observed the missing-helper import
failure, then implemented the extraction. The four checks and actual ordinary
performance contracts passed; its independent code persona found no issues.
A differential comparison of the original inline argument statements and new
helper matched exactly for 3,072 combinations spanning all 24 cases, both smoke
modes, effective repetition/time values and ownership/access/durability controls.
The remaining artifact/process/error transaction was deliberately retained.
All 20 checks passed on #171 head
`1095f8f9a97cc00b8a1fbb50d6be00bac78e8f1c`; it merged as
`d071c5dc215654f1a3407bd12e8f7e63b5261608`. Its delivered branches were removed.

The three independent patches were composed locally without changing any
published head. The integrated source/test tree at `ad6ebdc` was verified
identical to merged main at `d071c5d` and passed:

| Acceptance | Command / scope | Actual outcome |
|---|---|---|
| V01 | Warnings-as-errors `windows-debug` build; CTest `-L "unit\|cmake"` | 710 unit and 13 CMake checks passed |
| V02 | `windows-compatibility` build; CTest `-L "model\|compatibility\|crash"` | 18 checks passed |
| V03 | Optimized `windows-profiling` build; full performance label | 11 checks passed, including ordinary contracts/smoke, mutations and native CPU capture/calibration |
| V04 | Documentation validator and tool suite; changed C++ formatter; diff check | Validator/format clean and 13 documentation tests passed |
| V04 | Production/include/benchmark/build scan and generated Debug/profiling compile commands | No runtime assert, retired macro, collector include or dedicated target |
| V05 | Separate integration-persona pass of cross-child boundaries and source guidance | No actionable integration findings |

CTest selections used fail-on-empty behavior and retained JUnit reports.
The changed learning tours explain cleanup's lock stages and recovery's
acquisition-versus-publication boundary; the profiling guide explains pure
argument construction versus lifecycle ownership. Existing examples require no
API changes. The historical comment-remediation measurement record is retained
as evidence from its original date, not as advertising for retired targets.

These Windows runs do not establish native POSIX behavior, macOS capture,
sanitizers or GCC coverage. Each implementation's applicable remote checks
provide that platform evidence; the final child and reviewed completion handoff
are recorded here without promoting docs-only CI acknowledgements into runtime
evidence. A separate completion-documentation persona found no actionable
inaccuracies or omissions. The completion-record PR changes documentation only;
the reviewed code tree is unchanged.

The agreed bounded goal is complete: all 24 parent tasks and all three frozen
children have implementation or justified keep decisions, independent reviews,
verification evidence and persistent delivery. This is not a claim that every
future product feature or conceivable readability improvement is finished.
Further unrelated candidates belong to a new backlog; required fixes to this
delivered work do not.

Use task status and this document's outcome entries as the handoff, not memory
of previous conversation turns. Record actual checks after execution; do not
reuse the old baseline's test counts as evidence for the new revision. Removed
assertion-only death tests legitimately change the count.

Windows checks do not establish POSIX correctness or sanitizer coverage.
Environment-specific checks must run in their admitted environment; an
unavailable environment remains an explicit gap, not a passing result.
