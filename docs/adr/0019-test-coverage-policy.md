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
- Warn when an excluded line was executed, so stale exclusions surface.

### CI gate

A `coverage` job runs on Ubuntu with the default GCC for every push and pull
request:

1. Check out full history so the merge base with `main` is available.
2. Install pinned `gcovr==8.6` and `diff-cover==10.6.0`.
3. Configure, build, and test the `coverage` preset.
4. Print the gcovr summary and upload the Cobertura XML and HTML reports as an
   artifact.
5. Run `diff-cover` against `origin/main` with branch coverage and a required
   score of 100.

Every added or modified coverable line in `src/` or `include/` must execute,
and every branch on those lines must be taken, unless explicitly excluded.
Code that is not compiled by the Linux coverage build does not appear in the
report and is outside the gate. Deleted lines do not count.

Overall line and branch coverage are reported but not gated, so legacy gaps do
not block unrelated pull requests. Coverage rises as code is touched.

### Exclusions

Prefer removing uncoverable code over excluding it: delete impossible checks
and restructure logic when doing so does not reduce clarity or safety.

An exclusion is allowed only when no deterministic test can execute the code,
for example the implicit default exit of an exhaustive `switch` over values
that earlier validation already restricts. An exclusion is not allowed for a
reachable path merely because testing it needs a fault-injection double, a
crafted input, or a slower test.

Every marker carries its justification in the same comment:

```cpp
switch (type) {  // GCOVR_EXCL_BR_LINE: exhaustive switch over decoded types
```

- `GCOVR_EXCL_LINE: <reason>` excludes one line.
- `GCOVR_EXCL_BR_LINE: <reason>` excludes only the branches of one line.
- `GCOVR_EXCL_START: <reason>` and `GCOVR_EXCL_STOP` bracket a region.

Reviewers evaluate every new marker like production code.

### Local workflow

```bash
cmake --preset coverage
cmake --build --preset coverage
ctest --preset coverage
mkdir -p build/coverage/html
gcovr --txt-summary --cobertura build/coverage/coverage.xml \
  --html-details build/coverage/html/index.html
diff-cover build/coverage/coverage.xml --compare-branch=origin/main \
  --branch-coverage --fail-under=100
```

With Apple Clang on macOS, add `--gcov-executable "xcrun llvm-cov gcov"` to
the `gcovr` command. Clang and GCC can report slightly different branches;
the GCC report in CI is authoritative.

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
