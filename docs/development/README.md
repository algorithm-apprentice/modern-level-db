# Development Guide

This directory is the current contributor reference for Modern LevelDB.
Historical ADRs and research handoffs explain why mechanisms were chosen;
these pages describe how to build, validate, measure, and review the current
repository.

## Routes

| Need | Document |
|---|---|
| Start a change from a concept, contract, source index, and evidence map | [Document-first development](document-first-development.md) |
| Configure, build, and run the test tiers | [Building and testing](building-and-testing.md) |
| Understand CI and quality gates | [CI and quality gates](ci-and-quality-gates.md) |
| Run comparative benchmarks, selected workloads, and CPU capture | [Benchmarking and profiling](benchmarking-and-profiling.md) |
| Run reproducible native database stress campaigns locally | [Local database stress testing](local-stress-testing.md) |
| Follow code, comment, formatting, and review conventions | [Code style](code-style.md) |
| Complete textbook-oriented refactoring through small verified tasks | [Textbook improvement plan](textbook-improvement-plan.md) |
| Review source-comment accuracy, contracts, and learning gaps | [Comment audit](comment-audit.md) |
| Follow comment-only improvement batches, progress, and acceptance criteria | [Comment remediation plan](comment-remediation-plan.md) |

## Delivery rule

Changes follow the sequential pull-request workflow in
[ADR-0006](../adr/0006-sequential-pull-request-workflow.md):

1. Start from the relevant concept and establish its maintained contract,
   bounded source map, and evidence.
2. Deliver one coherent DAG node.
3. Run the smallest complete validation plus affected consumers.
4. Obtain the required independent review and close findings.
5. Merge the exact reviewed green revision before starting the next node.

Documentation-only work uses content/structure validation rather than
artificial product tests. Product behavior still requires TDD and the
repository's coverage, sanitizer, compatibility, fuzz, and benchmark gates.
The [document-first workflow](document-first-development.md) explains which
document class owns each kind of fact and when the concept index must change.

## Current and historical documents

The current procedures are in this directory. The retained
[`profiling-design.md`](../profiling-design.md) and
[`write-profiling-design.md`](../write-profiling-design.md) files are
historical research/implementation handoffs. They link back to the current
measurement procedure and preserve superseded schema evidence.
