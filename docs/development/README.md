# Development Guide

This directory is the current contributor reference for Modern LevelDB.
Historical ADRs and research handoffs explain why mechanisms were chosen;
these pages describe how to build, validate, measure, and review the current
repository.

## Routes

| Need | Document |
|---|---|
| Configure, build, and run the test tiers | [Building and testing](building-and-testing.md) |
| Understand CI and quality gates | [CI and quality gates](ci-and-quality-gates.md) |
| Run comparative benchmarks, selected workloads, diagnostics, and CPU capture | [Benchmarking and profiling](benchmarking-and-profiling.md) |
| Follow code, comment, formatting, and review conventions | [Code style](code-style.md) |

## Delivery rule

Changes follow the sequential pull-request workflow in
[ADR-0006](../adr/0006-sequential-pull-request-workflow.md):

1. Establish the relevant contract and evidence.
2. Deliver one coherent DAG node.
3. Run the smallest complete validation plus affected consumers.
4. Obtain the required independent review and close findings.
5. Merge the exact reviewed green revision before starting the next node.

Documentation-only work uses content/structure validation rather than
artificial product tests. Product behavior still requires TDD and the
repository's coverage, sanitizer, compatibility, fuzz, and benchmark gates.

## Current and historical documents

The current procedures are in this directory. The retained
[`profiling-design.md`](../profiling-design.md) and
[`write-profiling-design.md`](../write-profiling-design.md) files are
historical research/implementation handoffs. They link back to the current
measurement procedure and preserve superseded schema evidence.
