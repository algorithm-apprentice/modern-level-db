# ADR-0019: Test Coverage Policy

- Status: Proposed
- Date: 2026-09-23

## Context

[ADR-0005](0005-test-driven-development.md) requires test-first development,
but nothing measures which production code the tests execute. A local coverage
build during review of the WAL stream node exposed reachable but untested
paths: individual append failures, a close failure, a restarted fragmented
record, and a zero marker without a partial record. They were found only
because a reviewer asked about coverage.

New production code must therefore be executed by tests as completely as
practical. Code that no deterministic test can execute may be excluded, but
only explicitly and with a recorded reason.

## Requirements

| Need | Required behavior |
|---|---|
| Pull-request review | Fail CI when added or modified production code or its branches are not executed |
| Development | Reproduce the CI report locally before pushing |
| Exclusion review | Make every excluded line or branch visible in source together with its reason |
| Maintenance | Show overall coverage without blocking unrelated changes on legacy gaps |

No current need exists for hosted coverage services, badges, historical trend
storage, per-test attribution, MC/DC coverage, or mutation testing.

## Prior art

- **GCC gcov with gcovr:** native GCC instrumentation. gcovr filters sources,
  honors in-source exclusion markers, removes exception-only branches, and
  writes text, Cobertura XML, and HTML reports. Adopted.
- **LLVM source-based coverage:** precise region coverage and useful locally,
  but `llvm-cov` itself has no in-source exclusion markers. Not used for the
  CI gate.
- **lcov/genhtml:** equivalent markers through a separate Perl toolchain that
  adds nothing over gcovr. Rejected.
- **diff-cover:** compares a Cobertura report with the git diff and can treat
  partially covered branches as uncovered. Adopted for the changed-code gate.
- **Hosted patch coverage such as Codecov:** the same concept as diff-cover,
  but it requires an external service and an upload token. Rejected.
- **SQLite's 100% MC/DC branch coverage:** far beyond current needs, but its
  principle that untestable code needs explicit justification is adopted.

## Decision

### Coverage build

Add a `coverage` CMake preset: a Debug build with `--coverage` for compilation
and linking, warnings as errors, and only the `unit` test label. The preset
also passes `-fprofile-update=atomic`: concurrent unit tests update counters
from several threads, and non-atomic counters produce inconsistent, even
negative, gcov branch counts ([GCC bug 68080](https://gcc.gnu.org/bugzilla/show_bug.cgi?id=68080)).
The preset sets the instrumentation; no CMake option or project code is added.

### Report configuration

A repository-root `gcovr.cfg` defines the report for CI and local runs:

- Include only production code under `src/` and `include/`. Tests and
  third-party code are excluded.
- Honor only `GCOVR_`-prefixed exclusion markers.
- Remove compiler-generated exception branches and branches on lines without
  source code.
- Remove unexecuted lines that contain only braces or `else`. GCC and Clang
  attribute the exception cleanup of named locals to a function's closing
  brace, which therefore executes only when an exception propagates. Such
  lines carry no statements, and an unexecuted block still reports its body
  lines as uncovered.
- Remove the branches of lines that start with `assert(`. A failing assertion
  calls `abort()`, which terminates before gcov writes its counters, so not
  even a death test can record that branch. The asserted expression still
  executes and remains subject to line coverage.
- Warn when an excluded line was executed, so stale exclusions surface.

### CI gate

A `coverage` job runs for every push and pull request on the fixed
`ubuntu-24.04` image with GCC 13. The compiler is pinned because different GCC
versions emit different branch graphs, and exact branch counts appear in
exclusion markers.

1. Check out full history so the merge base with `main` is available.
2. Install pinned `gcovr==8.6` and `diff-cover==10.6.0`.
3. Configure, build, and test the `coverage` preset with `g++-13`.
4. Print the gcovr report with `gcov-13` and upload the Cobertura XML and HTML
   reports as an artifact. Any error logged by gcovr, such as an exclusion
   marker whose branch counts no longer match, fails the job.
5. Run `diff-cover` against `origin/main` with branch coverage and a required
   score of 100.

Every added or modified coverable line in `src/` or `include/` must execute,
and every branch on those lines must be taken, unless explicitly excluded.
Code that is not compiled by the Linux coverage build does not appear in the
report and is outside the gate. Deleted lines do not count.

The gate compares a change with its merge base on `main`, so it applies to
pull requests and feature-branch pushes. On `main` itself the comparison is
empty; changes reach `main` only through pull requests that already passed the
gate.

Overall line and branch coverage are reported but not gated, so legacy gaps do
not block unrelated pull requests. Coverage rises as code is touched.

### Exclusions

Prefer removing uncoverable code over excluding it: delete impossible checks
and restructure logic when doing so does not reduce clarity or safety.

For example, GCC guards the cleanup of a temporary with a flag when the
temporary is constructed conditionally: a member of a partially constructed
aggregate with several non-trivially destructible members, or a temporary with
a non-trivial destructor, such as a `Status`, in an operand of `&&`, `||`, or
`?:`. If such a temporary appears inside an expression that may throw, such as
an argument to `push_back`, the flag check becomes a branch that only an
allocation failure can take. Construct the aggregate in its own declaration
first, and give each such temporary its own statement. Likewise, when a
constructor may throw, GCC guards freeing the memory of a `new` expression
with a flag whose check only a throwing constructor can take; keep
constructors that `new` calls `noexcept`, and do fallible work after
construction.

An exclusion is allowed only when no deterministic unit test can execute the
code:

- The implicit default exit of an exhaustive `switch` over values that earlier
  validation already restricts.
- A size or count guard that only an input larger than 4 GiB, or more than
  2^32 elements, can trigger. Such inputs exceed the memory and time budget of
  the unit tier.

An exclusion is not allowed for a reachable path merely because testing it
needs a fault-injection double, a crafted input, or a slower test.

Every marker carries its justification in the same comment:

```cpp
switch (type) {  // GCOVR_EXCL_BR_WITHOUT_HIT: 1/5 implicit default of an exhaustive switch
```

- `GCOVR_EXCL_LINE: <reason>` excludes one line.
- `GCOVR_EXCL_START: <reason>` and `GCOVR_EXCL_STOP` bracket a region.
- `GCOVR_EXCL_BR_WITHOUT_HIT: <unhit>/<total> <reason>` excludes the branches
  of one line only when exactly `<unhit>` of `<total>` branches were not
  taken. Any other count is a gcovr error that fails CI, so a reachable branch
  that stops being covered cannot hide behind the marker.

Do not use `GCOVR_EXCL_BR_LINE`: it removes every branch of the line,
including reachable ones.

Reviewers evaluate every new marker like production code.

### Local workflow

```bash
cmake --preset coverage
cmake --build --preset coverage
find build/coverage -name '*.gcda' -delete
ctest --preset coverage
mkdir -p build/coverage/html
gcovr --txt-summary --cobertura build/coverage/coverage.xml \
  --html-details build/coverage/html/index.html
diff-cover build/coverage/coverage.xml --compare-branch=origin/main \
  --branch-coverage --fail-under=100 --include-untracked
```

Deleting `.gcda` files first prevents counters from earlier runs from being
merged into the report. `--include-untracked` makes diff-cover also measure
new files that are not yet committed; CI has no untracked sources.

With Apple Clang on macOS, add `--gcov-executable "xcrun llvm-cov gcov"` to
the `gcovr` command. Clang's gcov emulation does not label exception edges,
so `exclude-throw-branches` cannot remove them: calls in functions with
non-trivial cleanups can appear as half-covered branches, and closing braces
as unexecuted lines. Treat such local gaps as approximations. The report from
GCC in CI is authoritative.

## Consequences

- CI gains one Linux job for the instrumented build.
- Uncovered changed lines and branches are listed directly in the CI log.
- Coverage of legacy code improves when that code is modified, without a
  one-time backfill.
- Coverage measures execution, not correctness. The behavioral assertions
  required by ADR-0005 remain mandatory.
- Exclusion markers become reviewed parts of the source.

## References

- [gcovr documentation](https://gcovr.com/en/stable/)
- [gcovr exclusion markers](https://gcovr.com/en/stable/guide/exclusion-markers.html)
- [diff-cover](https://github.com/Bachmann1234/diff_cover)
- [GCC gcov](https://gcc.gnu.org/onlinedocs/gcc/Gcov.html)
- [How SQLite is tested](https://www.sqlite.org/testing.html)
