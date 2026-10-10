# CI and Quality Gates

[Development guide](README.md)

The primary workflow is `.github/workflows/ci.yml`. It runs for pull requests
and pushes. A first `changes` job validates routing and decides whether a
change is documentation-only.

## Documentation-only routing

Changes containing only `README.md`, `docs/documentation-manifest.json`, and
Markdown files under `docs/` run the routing/documentation-contracts job and
explicit acknowledgement paths instead of compiling every product tier. A
changed source, CMake, workflow, script, other JSON file, or unknown path
selects full CI.

The routing classifier must fail safe:

- malformed/unavailable event metadata selects full CI;
- new branches compare with the fetched default branch;
- pull requests use the complete branch diff;
- renames include both paths;
- unusual filenames remain NUL-delimited.

## Documentation quality gate

The dependency-free checker applies objective rules to the complete
documentation corpus on every pull request and push:

```console
python3 tests/tools/ci_changes_test.py
python3 tests/tools/check_documentation_test.py
python3 tools/check_documentation.py
```

Common checks cover local links and anchors, heading hierarchy and duplicate
anchors, balanced language-tagged fences, UTF-8/LF/final-newline format,
privacy-sensitive paths and addresses, immutable-evidence policy, manifest
classification, and complete ADR index metadata.

Manifest `current`/`historical` values describe document retention, not ADR
decision outcomes. Each numbered ADR's exact `- Status:` field is the
decision-lifecycle authority and must match the derived ADR index.

Current documents additionally validate configure/build/test presets, CMake
targets, Python entry points, generated artifact paths, and versioned report
schemas against repository sources. Historical ADRs and handoff documents do
not have to use current commands or schemas, but they still receive all common
structural, metadata, privacy, and link checks.

Platform execution remains in the existing product jobs:

| Documented procedure | Retained CI execution |
|---|---|
| POSIX and Windows Debug build/test; storage diagnostics | `unit` Linux/macOS/Windows matrix |
| POSIX compatibility/model/crash | `compatibility` |
| Windows compatibility/cross-open/process recovery | `windows-compatibility` |
| POSIX ordinary benchmark | `benchmark` |
| Windows ordinary benchmark | `windows-benchmark` |
| POSIX selected workloads and profiling contracts | `performance` Linux/macOS matrix |
| Windows selected workloads, diagnostics, calibration, symbols, and cleanup | `windows-performance` |

The checker verifies that each primary procedure remains documented and that
its mapped workflow job retains the corresponding command or platform marker.
Documentation-only acknowledgements do not claim those platform commands ran
for the documentation change.

## Product matrix

Full CI includes:

| Gate | Purpose |
|---|---|
| Linux/macOS/Windows Debug and Release units | Core behavior, consumers, and CMake integration |
| Portable CRC32C | Forced non-hardware checksum implementation |
| Coverage | GCC line/branch report plus changed-code 100% gate |
| Compatibility | Model, golden, differential, crash, and recovery evidence |
| Windows compatibility | Native Debug/Release cross-open and process recovery |
| ASan/UBSan and TSan | Memory/undefined/concurrency diagnostics |
| Fuzz | Bounded codec/parser campaigns |
| Ordinary benchmarks | Severe-regression gate on POSIX |
| Windows benchmark | Copied/mapped diagnostic baseline with bound policy |
| POSIX performance | Selected workload and profiling contracts |
| Windows performance | Selected workloads, diagnostics, calibration, epochs, symbols, and owned collector cleanup |

Required checks apply to the exact reviewed head. A just-pushed revision may
take time to register its checks; “no checks reported” is not a successful
result.

## Coverage policy

Coverage applies to production code in `src/` and `include/`. New or changed
production lines and branches must be executed unless excluded under
ADR-0019 for an explicitly justified deterministic-test or toolchain
limitation. Line/branch markers record their reason in the same comment. A
region records the reason at `GCOVR_EXCL_START`, while a bare
`GCOVR_EXCL_STOP` closes it. Coverage does not replace behavioral assertions,
models, compatibility, sanitizers, fuzzing, or native platform tests.

The diff-coverage comparison ignores whitespace-only line changes; wrapped
statements still count as changed lines. The 100% threshold and all build/test
jobs remain unchanged. This avoids requiring a legacy-coverage backfill for a
mechanical formatting migration; see
[ADR-0071](../adr/0071-four-space-cpp-formatting.md).

See [ADR-0019](../adr/0019-test-coverage-policy.md) for exact exclusion
contracts.

## Failure handling

- Reproduce the specific failing job locally where supported.
- Use the job's compiler, build type, preset, and feature flags.
- Do not weaken thresholds, skip tests, or add broad catches to make CI green.
- Preserve failure artifacts and logs when cleanup/ownership cannot be
  verified.
- Fix only the demonstrated defect and directly coupled callers/tests.

## Pull request evidence

A PR description should state:

- coherent DAG node/scope;
- relevant design or contract;
- independent review and resolved findings;
- exact local validation;
- important limitations/nonclaims;
- artifact/provenance details for measurement work.

Merge only the exact reviewed head after all required checks pass and the
worktree is clean.
