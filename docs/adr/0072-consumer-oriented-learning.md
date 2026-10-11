# ADR-0072: Consumer-Oriented Learning Documentation

- Status: Accepted

## Context

The learning path already follows storage-engine concepts rather than source
directory order. Its lessons have worked examples, source tours, self-checks,
and guided labs. A technical-education review found that the sequence and
wording are strong, but the core lessons compress complex mechanisms into
short architecture-style explanations.

A reader opening a lesson directly may not know:

- what problem the component exists to solve;
- which code is its consumer;
- what interface or contract the consumer relies on;
- which behavior can be understood before reading the implementation;
- what source and test form the smallest useful reading set.

Reading code before establishing those boundaries creates unnecessary
cognitive load. The project needs a consistent teaching frame that treats
interfaces as relative to a component's consumer, not only as public library
APIs.

## Decision

Lessons 01 through 09 use this progression:

```text
purpose
    -> consumer
    -> consumer-facing interface and observable behavior
    -> worked example
    -> internal implementation
    -> tradeoffs and failure boundaries
    -> source and test evidence
```

Each lesson begins with a lesson map that names:

- the problem or capability being taught;
- the primary consumer of the component or mechanism;
- the interface or contract the consumer sees;
- the internal implementation focus;
- the completion capability expected from the learner.

"Consumer-facing" is relative to the current layer. Applications consume the
public `Database` API; `DatabaseEngine` consumes WalWriter, MemTable, and
VersionSet; MemTable consumes SkipList and Arena; Table consumes blocks,
filters, files, and caches.

Source tours use a common schema:

| Entry point | Interface or symbol | Question to answer | Representative evidence |
|---|---|---|---|

The source tour remains bounded. It names stable responsibility boundaries,
not every helper in the call graph.

Each self-check includes at least one transfer question that asks the learner
to apply the contract to a new failure, ordering, ownership, or visibility
scenario.

Lessons 08 and 09 are marked advanced engineering lessons. They support the
implementation path but are not prerequisites for understanding the ordinary
LSM lifecycle on a first pass. Lesson 11 is an appendix/comparison, not another
required step in the core curriculum.

## Document authority

- Learning lessons own mental models, examples, and teaching sequence.
- Public headers and reference documents own user-visible behavior.
- Architecture owns current module and dependency boundaries.
- ADRs own design rationale and durable alternatives.
- The concept-to-code index owns cross-concept navigation.
- Source comments own local preconditions and invariants.

The lessons link to those authorities instead of copying every normative
detail.

## Consequences

- A reader can understand what a component is for before opening its source.
- Internal components can be taught through their own consumer-facing
  contracts without pretending they are public APIs.
- Lesson structure becomes predictable across the curriculum.
- Source tours become more actionable for implementation readers.
- Maintainers must update lesson maps and source tours when responsibility
  moves between modules.
- Some repetition is intentional: a short interface summary at the start of a
  lesson reduces the larger cost of reconstructing context from later prose.

## Non-goals

- Turning the learning path into generated API documentation.
- Explaining every helper, field, or test.
- Replacing current user reference, architecture, ADR, or development guides.
- Making the curriculum a zero-prerequisite C++ or operating-systems textbook.
- Removing exact terminology or implementation evidence in favor of loose
  analogies.

## Acceptance criteria

- Lessons 01–09 have a consumer-oriented lesson map.
- Lessons 01–09 use the common source-tour schema.
- Lessons 01–09 include a transfer-oriented self-check.
- Lessons 08 and 09 are visibly marked advanced.
- Lesson 11 is visibly marked as an appendix.
- The learning README explains the consumer-oriented structure.
- Documentation validation and its mutation tests pass.

## References

- [Documentation information architecture](0070-documentation-information-architecture.md)
- [Document-first development](../development/document-first-development.md)
- [Concept-to-code index](../learning/concept-index.md)
- [Learning path](../learning/README.md)
