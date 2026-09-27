# ADR-0048: Documentation-Only CI Routing

## Status

Accepted design. Merge this design before its separate implementation.

## Context

ADR-0005 already separates fast unit feedback from expensive integration,
sanitizer, fuzz, and performance checks. Pure documentation changes still
trigger all of those builds in `ci.yml`, including design-only pull requests
that deliberately precede implementation.

GitHub documents that workflow-level path filters can leave required checks
pending, while a skipped job reports a successful check. Job conditions are
evaluated before matrix expansion, so skipping a matrix job is not a reliable
way to retain each existing expanded check name.

The change must reduce unnecessary work without weakening checks for code,
test, dependency, or workflow changes, or changing branch-protection settings.

## Decision

### Conservative path allowlist

A nonempty change is documentation-only when every changed path is either:

- The root `README.md`.
- A Markdown file ending in `.md` under `docs/`, including nested ADRs.

These paths are documentation, not inputs to compilation or runtime tests.
Do not skip for arbitrary Markdown files elsewhere, scripts or other files
under `docs/`, dependencies, tests, CMake, or `.github/workflows/`.
Mixed changes always run the existing full CI matrix.

Use repository-relative Git paths without case folding or normalization that
could conceal a different path. Renames must account for both the old and new
paths: a source file renamed into documentation is still a code change.
Use NUL-delimited Git output, with rename detection disabled, so spaces,
newlines, and Git rename heuristics cannot hide affected paths.

### Event comparison and failure behavior

Add a small Python-standard-library helper, `tools/ci_changes.py`, used by a
lightweight `changes` job. Checkout full history once for this job and compute
the complete path list locally; do not rely on an API's limited file list or
the latest commit's paths.

| Event | Comparison |
|---|---|
| Pull request | Merge base of the event's base/head commits through the head commit |
| Existing branch push | Event `before` tree through event `after` tree, including multi-commit and force pushes |
| New non-default branch push | Merge base of the head and fetched default branch through the head |
| New default branch, tag, unsupported event, missing history or invalid event metadata | Log the reason and require full CI |

A documentation follow-up commit on a code-changing PR must not make its
PR run documentation-only. Its push run may skip when that push changes only
documentation, but the PR run considers the entire PR diff.

Push and PR checks must therefore have distinct names: preserve the existing
PR check names and prefix push check names with `push / `. Required checks
do not distinguish these events by name, so a successful no-op push check
must not satisfy the corresponding full PR check. This small naming change
is necessary to safely keep incremental push classification.

Validate commit identifiers before passing them to Git, use argument arrays
without shell interpolation, and require valid NUL-delimited output. Empty
diffs conservatively run full CI. Comparison failures produce an explicit
warning and select full CI, not an apparent documentation success.
Failures to execute the routing job or write its output remain visible
workflow failures.

The helper writes only `docs_only=true` or `docs_only=false` to
`GITHUB_OUTPUT`; event data and filenames are never interpolated into a shell
command or an output directive.

### Workflow and required-check behavior

Keep `ci.yml` triggered on the existing push and pull-request events without
path filters or commit-message skip directives. The new `changes` job runs
its fast classification tests, classifies the event, and exports the decision.
It needs only the existing `contents: read` permission; no third-party change
filter action or package is added.

Every existing job depends on `changes`. Only a successful routing job with
the exact output `true` permits skipping work. On a missing or failed
classification result/output, full jobs still run unless the workflow was
cancelled.

- Non-matrix jobs keep their PR names and skip at job level for documentation.
- `unit` and `performance` retain all existing matrix combinations and PR names.
  For documentation only, each runs a short acknowledgement on Ubuntu and
  skips checkout, dependency installation, configuration, builds, tests, and
  artifact upload. No macOS or Windows worker is needed for these no-op checks.
- With any other change, matrix runners, commands, timeouts, test coverage,
  artifact behavior, and all non-matrix gates stay as before.

The lightweight `changes` check and matrix acknowledgements finish normally;
other existing checks are explicitly skipped rather than absent because the
whole workflow was filtered out. Push uses the same event-specific prefix
for all checks, including `changes`. No new branch-protection requirements
are installed by this change.

The weekly/manual `extended-hardening` workflow is unchanged. Its scheduled
campaigns run independently of which files changed most recently, and its
existing workflow-file PR trigger remains intact.

### Tests and delivery evidence

Use `tests/tools/ci_changes_test.py` with Python's existing `unittest` pattern.
It runs without compiling the engine, installing packages, network requests,
or sleeps. Cover:

1. Allowed documentation, code-only, mixed, outside-allowlist Markdown,
   unusual filenames, empty changes, and large complete path lists.
2. PR history containing code followed by documentation, including a
   diverged base; the latest commit alone is not sufficient.
3. Multi-commit pushes, new branches, force-push tree differences,
   document deletion/renames, and renames between code and documentation.
4. Missing/invalid commit metadata, unavailable revisions, malformed Git
   output, unsupported events, and visible conservative fallback.
5. Exact output-file behavior; output-write failures must not claim success.

Inspect the workflow configuration and verify real GitHub executions, not
only the helper: a code/workflow-changing PR must execute the full existing
checks under the existing PR names. A documentation-only follow-up push must
skip expensive work, publish the corresponding `push / ` names, and finish
successfully. Its PR run must still execute full CI because the PR contains
the implementation, without any name shared between the two events.

Record the observed routing and matrix names before merge. Deliver through a
separate implementation PR after this design, with bounded review.

The bounded design review identified the risk of same-name push checks
satisfying full PR checks. The event-specific push names above resolve that
finding while preserving the existing PR merge-gate identities.

## Scope

Do not change test algorithms, split CMake tests out of the unit job, tune
timeouts, add concurrency cancellation, change action versions, alter
sanitizer/fuzz campaigns, or start a performance optimization in this slice.
Those are separate decisions.

## References

- [ADR-0005 test tiers and documentation validation](0005-test-driven-development.md)
- [GitHub job conditions and skipped checks](https://docs.github.com/en/actions/how-tos/write-workflows/choose-when-workflows-run/control-jobs-with-conditions)
- [Workflow syntax and path-filter limitations](https://docs.github.com/en/actions/reference/workflows-and-actions/workflow-syntax)
