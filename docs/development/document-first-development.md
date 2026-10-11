# Document-First Development

Modern LevelDB uses documentation as the control plane for understanding and
changing the engine. Code remains the executable implementation, but a
contributor should not need to reconstruct the whole architecture from source
before making a focused change.

The working path is:

```text
question or concept
    -> learning model and concept index
    -> current contract and architecture boundary
    -> bounded source entry points
    -> representative tests and evidence
    -> implementation
    -> updated documentation index
```

This is not a requirement to write speculative prose before every edit.
Documentation should reduce the active mental model, preserve decisions, and
identify evidence. It must not duplicate every implementation detail.

## Start from the concept

Use the [concept-to-code index](../learning/concept-index.md) before searching
the repository. It provides:

- the learning lesson that explains the mechanism;
- the current contract or architecture document;
- a bounded set of source entry points;
- representative tests and higher-level evidence;
- the ADRs that explain why the boundary exists.

Read the entry-point function and its focused tests before following callees.
Open additional files only when the behavior crosses a responsibility boundary
identified by the documentation.

## Choose the authoritative document

Different document classes answer different questions. Update the authority
that owns the changed fact rather than repeating it everywhere.

| Question changed by the work | Document to update |
|---|---|
| What does a library user observe or need to do safely? | `docs/reference/` and public-header comments |
| How is the current engine divided into modules and dependency directions? | `docs/architecture.md` |
| How should a contributor build, test, measure, or deliver the change? | `docs/development/` |
| How should a learner understand the stable concept? | `docs/learning/` |
| Why did the project choose or reject a durable design direction? | A new ADR or a dated amendment/outcome on an existing ADR |
| What local precondition is required to read a function correctly? | A nearby source comment |
| Which code and tests implement a concept? | `docs/learning/concept-index.md` and the lesson's source tour |

Not every patch needs a new ADR. Create one when the decision changes an
ownership model, persistent format, public contract, durability boundary,
concurrency model, module responsibility, or long-lived performance policy.

## Change workflow

### 1. State the concept and contract

Write one short problem statement:

```text
Concept:
Observed mismatch:
Current authority:
Invariant that must remain:
```

If no current authority describes the behavior, improve the relevant learning,
reference, architecture, or development document before treating an incidental
code pattern as the design.

### 2. Build a bounded change map

From the concept index and source tour, record:

```text
Implementation entry points:
Representative unit tests:
Cross-cutting evidence:
Relevant ADRs:
```

This list is a reading boundary, not a prediction that every listed file must
change. Add a file only when the call or ownership path crosses into it.

### 3. Review the design before editing

Compare the current implementation with:

- its documented invariant;
- the pinned LevelDB behavior when parity is relevant;
- Modern LevelDB's intentional ownership, error, durability, and platform
  differences;
- the failure and crash states represented by existing tests.

Decide which layer owns validation, lifetime, synchronization, and sticky
failure. Avoid adding a defensive check at every layer when one checked
boundary already establishes a trusted internal path.

### 4. Change tests and implementation together

Use the smallest test that proves the contract, then expand to affected
consumers. A change to a persistent or crash boundary normally needs more than
a unit round trip: use golden, compatibility, crash, model, fuzz, or sanitizer
evidence as directed by the concept index and
[CI guide](ci-and-quality-gates.md).

### 5. Reconcile documentation

Before review:

- update current reference if observable behavior changed;
- update learning prose if the mental model or invariant changed;
- update the concept index or source tour if responsibility moved;
- update architecture if a module or dependency boundary changed;
- add or amend an ADR when the durable decision changed;
- remove stale names and contracts from retained historical summaries without
  rewriting historical rationale as if it were new.

### 6. Review from the documents

Reviewers should be able to start from the PR's concept/contract link, follow
the bounded code index, and verify the evidence without rediscovering the
whole subsystem.

The PR description should answer:

```text
Concept and authority:
Design decision:
Source boundaries changed:
Evidence:
Documentation/index updates:
```

Use `N/A` when a field genuinely does not apply; do not invent an ADR or broad
test tier for a local change.

### 7. Merge only a coherent exact revision

Use the sequential workflow in
[ADR-0006](../adr/0006-sequential-pull-request-workflow.md). Merge the exact
reviewed revision only after the relevant code and documentation gates pass.

## Maintenance rules for source indexes

- Link stable entry points and representative tests, not every helper or every
  call site.
- Prefer a directory link only when the directory itself is the meaningful
  boundary, such as `tests/crash/` or `fuzz/`.
- Keep one normative contract. Learning pages explain; reference and public
  headers define user behavior; architecture defines module boundaries; ADRs
  preserve rationale.
- Update links in the same PR that moves or renames responsibility.
- Remove stale checked/trusted, ownership, or error terminology when the code
  boundary changes.
- Do not turn the concept index into a generated call graph. Its purpose is to
  reduce cognitive load and identify the next bounded reading set.

## Worked examples

### Trusted write-path boundary

Start with [Memory and MVCC](../learning/03-memory-and-mvcc.md) and
[WAL and recovery](../learning/04-wal-and-recovery.md). The concept index
points to SkipList, MemTable, WriteBatch, WalWriter, recovery, and their focused
tests. ADR-0060 provides the parity rationale. The design review can then decide
where external validation ends and trusted insertion begins without reading
unrelated table or compaction code.

### WAL close exception during rotation

Start with [WAL and recovery](../learning/04-wal-and-recovery.md), then open
`WalWriter::Close`, `DatabaseEngine::SwitchMemTable`, and their two focused
test files. ADR-0018 owns the stream contract; ADR-0037 owns the rotation state
transition. The bounded map makes the exception-safety gap visible without
loading the complete database engine.

## Definition of done

A document-first change is complete when:

- the maintained contract and the implementation agree;
- the concept index still reaches the correct entry points and evidence;
- source tours contain no stale names or responsibility claims;
- tests prove the changed behavior at the appropriate layer;
- the documentation checker passes;
- the exact reviewed commit passes the required product gates.
