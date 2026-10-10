# Code Style

This guide defines the shared C++ style for Modern LevelDB. It applies to
production code, tests, and examples. Use English for identifiers, comments,
documentation, ADRs, and commit messages.

## Naming

PascalCase capitalizes the first letter of each word without separators.
snake_case uses lowercase words separated by underscores.

| Category | Convention | Examples |
|---|---|---|
| Types and type aliases | PascalCase | `Error`, `ErrorCode`, `ByteView`, `Result` |
| Functions and ordinary member functions | PascalCase | `AsBytes`, `ConsumeFixed32`, `ToString` |
| Simple value accessors | snake_case | `code()`, `message()`, `location()` |
| Scoped enum members | PascalCase, no `k` prefix | `ErrorCode::NotFound`, `ErrorCode::Corruption` |
| Named compile-time constants (`constexpr`) | PascalCase, no `k` prefix | `PayloadBits`, `MaxBytes`, `EncodedSize` |
| Parameters and local variables | snake_case | `input`, `remaining_bits`, `maximum_payload` |
| Private non-static data members | snake_case with a trailing underscore | `code_`, `message_`, `location_` |
| Template type parameters | PascalCase or a conventional single capital letter | `UInt`, `T` |
| Namespaces | snake_case | `modern_leveldb` |
| C++ filenames | snake_case with `.h` or `.cc` | `result.h`, `coding.cc`, `coding_test.cc` |
| Include guards | UPPER_SNAKE_CASE with a trailing underscore | `MODERN_LEVELDB_BASE_RESULT_H_` |
| GoogleTest suites and cases | PascalCase | `CodingTest`, `RejectsOverflowingVarint32` |

Do not add a `k` prefix to enum members or compile-time constants. The
`constexpr` qualifier and enum scope already communicate their roles.
Runtime `const` locals still use snake_case; immutability alone does not make
a local variable a named compile-time constant.

```cpp
constexpr std::size_t PayloadBits = 7;
std::size_t index = 1;
const std::size_t shift = index * PayloadBits;
```

Simple accessors intentionally follow the value-like style common in the C++
standard library. Use `error.code()` rather than `error.Code()`, while
operations such as `error.ToString()` use PascalCase.

This is a project-specific C++ convention, not a wholesale adoption of C#
or Google naming rules. Keep standard-library and external API spellings
unchanged. Diagnostic strings such as `not_found`, comparator identities, and
persistent-format identifiers are not governed by C++ identifier naming rules.

## Formatting

[`.clang-format`](../../.clang-format) is the source of truth for formatting:

- Google-based layout with four-space block and continuation indentation.
  Wrapped arguments may still align with the first argument.
- Spaces only, with a four-column tab width.
- Access labels such as `public:` and `private:` aligned with the enclosing
  class declaration.
- A 100-column limit.
- Opening braces on the declaration or control-flow line.
- Left-aligned pointer and reference markers, such as `Error*` and `ByteView&`.
- Case-sensitive include sorting.

The indentation, tab policy, and access-label alignment reference
[Catch2's configuration](https://github.com/catchorg/Catch2/blob/04382af4c640d801d332191528d11bd7ae5819f3/.clang-format#L23-L41),
not its complete formatting style. Other layout choices retain the existing
Google-based conventions; see
[ADR-0071](../adr/0071-four-space-cpp-formatting.md).

```cpp
class Reader {
public:
    void Reset() {
        position_ = 0;
        ready_ = false;
    }

private:
    std::size_t position_ = 0;
    bool ready_ = false;
};
```

Use the formatter on changed C++ files rather than manually adjusting layout.
For example, from the repository root:

```bash
clang-format -i src/base/coding.cc tests/unit/base/coding_test.cc
```

On macOS, `xcrun clang-format` can be used when the formatter is provided by
Xcode. Avoid reformatting unrelated files in a behavior or naming change.

`clang-format` controls layout, not identifier naming. The current
[`.clang-tidy`](../../.clang-tidy) configuration also does not enforce the naming
table; naming consistency must be checked during review.

## API and comments

Follow [ADR-0004](../adr/0004-errors-ownership-and-runtime.md) for the error,
ownership, and runtime model.

- Use `Result<T>` for fallible operations returning a value, and `Status` for
  fallible operations without a success payload.
- Mark functions whose result must not be ignored with `[[nodiscard]]`.
- Use `noexcept` only when the implementation and its callees uphold that
  guarantee.
- Use RAII for ownership and document borrowed-view lifetimes.
- Explain non-obvious contracts and invariants in comments; do not narrate
  straightforward code.

For performance-sensitive internal loops, establish validity at explicit
boundaries and rely on documented object/loop invariants afterward. Do not
repeat recoverable-error checks for states those invariants exclude. Keep
external-input and I/O failures explicit, and use debug assertions for proven
internal preconditions. Any unchecked helper needs identified callers and a
complete validity/lifetime argument; see
[ADR-0046](../adr/0046-validated-block-decoding-experiment.md) for the historical
proof pattern and [ADR-0055](../adr/0055-leveldb-block-iterator-parity.md) for the
current lazy, checked block-decoding boundary.

### Comments as a learning layer

Keep the layers complementary: lessons explain concepts and worked examples,
headers state caller obligations, implementations explain invariant transitions,
tests provide executable evidence, and ADRs retain decision history.

Organize comments around this engine's representation, visibility, persistence,
ownership, concurrency, and reclamation. Explain the local invariant instead
of using another implementation's function name as its explanation. Keep
upstream provenance only where it identifies a format contract or independent
test oracle; do not erase compatibility identities or fixture origins.

At a core module or type, briefly identify its role and the important
representation, ownership, or synchronization model. At a declaration, explain
non-obvious preconditions, absence/error meanings, borrowed-view invalidation,
and failure/exception state. Beside a subtle transition, explain why publication,
retention, or durability remains correct rather than narrating assignments.

Use summary-first English prose, separating prerequisites and lifetime/error
rules into paragraphs when needed. Name the retaining owner, protecting mutex,
or invalidation event; "thread-safe" or "valid" alone is not a complete contract.
Distinguish owned wrappers from owned backing storage and checked boundaries
from trusted internal inputs.

Link concepts to repository-relative lesson paths or stable test/symbol names
when the next reading step is otherwise difficult to find. Do not embed source
line numbers or repeat complete lessons and historical performance conclusions.
Annotate representative golden bytes, thresholds, schedules, and model limits
when their reasoning is opaque; do not restate descriptive test names.

No comment quota, mandatory comment on every function, or documentation generator
is required. Keep straightforward accessors, wrappers, and build commands terse.
Preserve `GCOVR_EXCL_*`, `NOLINT`, and formatter directives and their reasons.
Check every new contract against its actual callers and implementation; an
unsupported guarantee is worse than missing prose.

## Review and changes

Use behavior-oriented test names, as in
`CodingTest.RejectsTruncatedVarintWithoutConsumingInput`.
Follow [ADR-0005](../adr/0005-test-driven-development.md) for the TDD workflow.
Behavior-preserving internal renames belong to the refactoring step: keep
the existing tests green before and after the change rather than adding
tests that inspect private identifier spellings.

Coverage exclusions follow [ADR-0019](../adr/0019-test-coverage-policy.md):
prefer deleting unreachable code, and give every `GCOVR_EXCL_LINE`,
`GCOVR_EXCL_BR_WITHOUT_HIT`, and `GCOVR_EXCL_START` marker its justification
in the same comment. A bare `GCOVR_EXCL_STOP` closes the documented region.
For example:
`// GCOVR_EXCL_BR_WITHOUT_HIT: 1/5 implicit default of an exhaustive switch`.

Keep style changes scoped to the requested change. Update this guide before
introducing a different convention, and retain the sequential pull-request
workflow from [ADR-0006](../adr/0006-sequential-pull-request-workflow.md).
