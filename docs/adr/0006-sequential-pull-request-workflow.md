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

## Review gate

Before asking the project owner to review a pull request:

1. Obtain independent reviews from personas relevant to the change, such as
   C++/storage correctness, build/integration, and architecture/test contracts.
   Reviews may run concurrently; implementation and fixes remain sequential.
2. Treat each finding as a candidate, not an instruction. Check the actual
   contract, triggering code path, realistic impact, and fix cost. Reproduce
   the issue where feasible; a precise static counterexample is also evidence.
3. Accept demonstrated defects and justified, directly related improvements.
   Decline unsupported claims and speculative redesigns with an explicit reason.
   Passing tests alone neither prove correctness nor invalidate a sound finding.
4. Add a regression test for each accepted behavior bug, observe its failure,
   and apply the smallest complete fix.
5. Reuse the reviewers for focused follow-up passes after fixes. Repeat until
   no actionable findings remain unresolved. Report a blocked defect as blocked,
   not as a clean review.
6. Record the review scope, accepted fixes, declined findings and reasons, and
   actual validation commands in the PR handoff before requesting owner review.

A clean review means no remaining actionable findings were identified in the
reviewed revision; it is not a proof that the software has no defects.

## Consequences

- Review feedback is incorporated before dependent nodes are designed in
  detail.
- The implementation DAG remains the long-term plan, while pull requests are
  the execution and approval boundary.
- Throughput is intentionally traded for clearer causality and lower rework
  risk.
