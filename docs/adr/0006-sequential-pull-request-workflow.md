# ADR-0006: Sequential Pull Request Workflow

- Status: Accepted
- Date: 2026-09-21

## Context

Storage-engine changes benefit from narrow review boundaries. Developing
multiple DAG branches at once would make it harder to identify which change
introduced a format, correctness, or performance regression.

The project owner reviews development one pull request at a time.

## Decision

All development is delivered through sequential pull requests.

- A pull request contains one coherent DAG slice.
- Adjacent foundational nodes may share a pull request only when separating
  them would leave the repository unbuildable or remove the behavior needed to
  review the slice.
- Every production behavior in the pull request includes its TDD unit tests.
- The pull request description records the implemented DAG nodes and the
  validation command.
- Work on the next pull request does not begin until the current pull request
  has been reviewed and merged.
- Unrelated implementation branches are not developed in parallel.

## Consequences

- Review feedback is incorporated before dependent nodes are designed in
  detail.
- The implementation DAG remains the long-term plan, while pull requests are
  the execution and approval boundary.
- Throughput is intentionally traded for clearer causality and lower rework
  risk.
